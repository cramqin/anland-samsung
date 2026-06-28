package com.anland.consumer;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.CountDownTimer;
import android.text.Editable;
import android.text.TextWatcher;
import android.text.InputType;
import android.util.Log;
import android.util.SparseArray;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;


public class SettingsActivity extends Activity {
    private static final String TAG = "AnlandSettings";
    private static final String PREFS_NAME = "anland_settings";
    private static final String KEY_BOUND_KEYCODE = "bound_keycode";
    private static final String KEY_SOCKET_PATH = "socket_path";
    private static final String KEY_USE_ROOT = "use_root";
    private static final String KEY_MIC_ENABLED = "mic_enabled";
    private static final String KEY_CAMERA_ENABLED = "camera_enabled";
    private static final String KEY_SPEAKER_LATENCY_MS = "speaker_latency_ms";
    private static final String KEY_MIC_LATENCY_MS = "mic_latency_ms";
    private static final String KEY_ACCESSIBILITY_ENABLED = "accessibility_key_intercept";
    private static final String KEY_EXTRA_KEYS_ENABLED = "extra_keys_bar";
    private static final String KEY_AUTO_SHOW_EXTRA_KEYS = "auto_show_extra_keys";
    private static final String DEFAULT_SOCKET_PATH = "/data/local/tmp/display_daemon.sock";
    private static final int UNBOUND = -1;

    // Latency presets: label shown in the spinner -> target buffer in ms (0 = auto).
    private static final int[] LATENCY_MS = {0, 1, 3, 5, 10, 20};
    private static final String[] LATENCY_LABELS = {
        "Auto (engine default)", "Ultra Low (~1 ms)", "Low (~3 ms)",
        "Balanced (~5 ms)", "Safe (~10 ms)", "Relaxed (~20 ms)"
    };

    // Resolution presets. Index 0 is a no-op placeholder; fixed presets carry
    // explicit dimensions, "Screen ×f" entries are computed from the panel size
    // at selection time (see resolvePreset / scaleScreen).
    private static final String[] RES_PRESET_LABELS = {
        "Preset…",
        "Auto (0×0, native)",
        "4K (3840×2160)", "2K (2560×1440)", "1080p (1920×1080)",
        "720p (1280×720)", "480p (854×480)",
        "Screen × 1.0", "Screen × 0.8", "Screen × 0.75",
        "Screen × 0.5", "Screen × 0.25"
    };

    private Button bindButton;
    private TextView statusText;
    private CountDownTimer listenTimer;
    private boolean isListening = false;

    // Android keycode → human-readable name
    private static final SparseArray<String> KEY_NAMES = new SparseArray<>();
    static {
        KEY_NAMES.put(KeyEvent.KEYCODE_VOLUME_UP, "Volume Up");
        KEY_NAMES.put(KeyEvent.KEYCODE_VOLUME_DOWN, "Volume Down");
        KEY_NAMES.put(KeyEvent.KEYCODE_VOLUME_MUTE, "Volume Mute");
        KEY_NAMES.put(KeyEvent.KEYCODE_POWER, "Power");
        KEY_NAMES.put(KeyEvent.KEYCODE_CAMERA, "Camera");
        KEY_NAMES.put(KeyEvent.KEYCODE_HEADSETHOOK, "Headset Hook");
        KEY_NAMES.put(KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, "Media Play/Pause");
        KEY_NAMES.put(KeyEvent.KEYCODE_MEDIA_NEXT, "Media Next");
        KEY_NAMES.put(KeyEvent.KEYCODE_MEDIA_PREVIOUS, "Media Previous");
        KEY_NAMES.put(KeyEvent.KEYCODE_BRIGHTNESS_UP, "Brightness Up");
        KEY_NAMES.put(KeyEvent.KEYCODE_BRIGHTNESS_DOWN, "Brightness Down");
        KEY_NAMES.put(KeyEvent.KEYCODE_HOME, "Home");
        KEY_NAMES.put(KeyEvent.KEYCODE_BACK, "Back");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.WHITE);
        
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);

        // Title
        TextView title = new TextView(this);
        title.setText("Settings");
        title.setTextSize(24);
        title.setTypeface(null, Typeface.BOLD);
        title.setGravity(Gravity.START);
        title.setPadding(0, 0, 0, dp(32));
        root.addView(title);

        // Bind key section
        TextView bindLabel = new TextView(this);
        bindLabel.setText("Virtual Keyboard Key");
        bindLabel.setTextSize(16);
        bindLabel.setTypeface(null, Typeface.BOLD);
        bindLabel.setPadding(0, 0, 0, dp(8));
        root.addView(bindLabel);

        statusText = new TextView(this);
        statusText.setTextSize(14);
        statusText.setTextColor(Color.GRAY);
        statusText.setPadding(0, 0, 0, dp(16));
        root.addView(statusText);

        bindButton = new Button(this);
        bindButton.setText("Bind Virtual Keyboard Key");
        bindButton.setOnClickListener(v -> startListening());
        root.addView(bindButton);

        // === Accessibility key intercept switch ===
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        Switch accessibilitySwitch = new Switch(this);
        accessibilitySwitch.setText("Accessibility Key Interception");
        accessibilitySwitch.setTextSize(14);
        accessibilitySwitch.setPadding(0, dp(16), 0, 0);
        accessibilitySwitch.setChecked(prefs.getBoolean(KEY_ACCESSIBILITY_ENABLED, false));
        accessibilitySwitch.setOnCheckedChangeListener((v, checked) -> {
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                .putBoolean(KEY_ACCESSIBILITY_ENABLED, checked).apply();
            if (checked) {
                KeyInterceptor.launch(SettingsActivity.this);
            } else {
                KeyInterceptor.shutdown(false);
            }
        });
        root.addView(accessibilitySwitch);

        TextView accessibilityHint = new TextView(this);
        accessibilityHint.setText("Intercept function keys (Fn, F1-F12 etc.) via "
            + "AccessibilityService. Enable this if external keyboard Fn combos "
            + "don't reach the desktop. Requires Accessibility permission granted "
            + "in system Settings > Accessibility (one-time).");
        accessibilityHint.setTextSize(12);
        accessibilityHint.setTextColor(Color.GRAY);
        accessibilityHint.setPadding(0, dp(4), 0, dp(8));
        root.addView(accessibilityHint);

        // === Extra-keys bar switch ===
        Switch extraKeysSwitch = new Switch(this);
        extraKeysSwitch.setText("Extra Keys Bar (special keys)");
        extraKeysSwitch.setTextSize(14);
        extraKeysSwitch.setPadding(0, dp(16), 0, 0);
        extraKeysSwitch.setChecked(prefs.getBoolean(KEY_EXTRA_KEYS_ENABLED, false));
        extraKeysSwitch.setOnCheckedChangeListener((v, checked) ->
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                .putBoolean(KEY_EXTRA_KEYS_ENABLED, checked).apply());
        root.addView(extraKeysSwitch);

        TextView extraKeysHint = new TextView(this);
        extraKeysHint.setText("Show a Termux-style bottom bar with ESC/TAB/CTRL/ALT/"
            + "arrows/PgUp etc. The display area shrinks to make room; the bar rises "
            + "together with the soft keyboard. Takes effect on next return to the "
            + "desktop view.");
        extraKeysHint.setTextSize(12);
        extraKeysHint.setTextColor(Color.GRAY);
        extraKeysHint.setPadding(0, dp(4), 0, dp(8));
        root.addView(extraKeysHint);

        // === Auto-show extra keys with keyboard ===
        Switch autoShowSwitch = new Switch(this);
        autoShowSwitch.setText("Auto-show extra keys with keyboard");
        autoShowSwitch.setTextSize(14);
        autoShowSwitch.setPadding(0, dp(16), 0, 0);
        autoShowSwitch.setChecked(prefs.getBoolean(KEY_AUTO_SHOW_EXTRA_KEYS, true));
        autoShowSwitch.setOnCheckedChangeListener((v, checked) ->
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                .putBoolean(KEY_AUTO_SHOW_EXTRA_KEYS, checked).apply());
        root.addView(autoShowSwitch);

        TextView autoShowHint = new TextView(this);
        autoShowHint.setText("When ON, extra keys bar appears automatically with the soft "
        + "keyboard and hides when it closes. When OFF, bar stays visible until "
        + "manually toggled via settings.");
        autoShowHint.setTextSize(12);
        autoShowHint.setTextColor(Color.GRAY);
        autoShowHint.setPadding(0, dp(4), 0, dp(8));
        root.addView(autoShowHint);

        addConnectionSection(root);

        addResolutionSection(root);

        scroll.addView(root);
        setContentView(scroll);

        // Edge-to-edge is enforced on Android 15+ (targetSdk 36): the system no
        // longer auto-resizes the window for the IME, so a manifest "adjustResize"
        // is ignored and the soft keyboard overlaps the bottom EditTexts. Take over
        // inset handling and pad the scrollable content by the system-bar + IME
        // insets ourselves, so the ScrollView can scroll the focused field above
        // the keyboard. Base padding (dp(24)) is preserved on all edges.
        getWindow().setDecorFitsSystemWindows(false);
        final int base = dp(24);
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            Insets in = insets.getInsets(
                WindowInsets.Type.systemBars() | WindowInsets.Type.ime());
            v.setPadding(base + in.left, base + in.top,
                         base + in.right, base + in.bottom);
            return insets;
        });

        updateStatus();
    }

    // Connection settings: a custom daemon socket path and a "connect with root"
    // toggle. In root mode the app launches the bundled helper via `su -c`, which
    // connects to the socket and passes the fd back (see MainActivity).
    private void addConnectionSection(LinearLayout root) {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);

        TextView header = new TextView(this);
        header.setText("Connection");
        header.setTextSize(16);
        header.setTypeface(null, Typeface.BOLD);
        header.setPadding(0, dp(32), 0, dp(8));
        root.addView(header);

        // Socket path
        TextView sockLabel = new TextView(this);
        sockLabel.setText("Daemon socket path");
        sockLabel.setTextSize(14);
        sockLabel.setTextColor(Color.GRAY);
        sockLabel.setPadding(0, 0, 0, dp(4));
        root.addView(sockLabel);

        EditText socketInput = new EditText(this);
        socketInput.setSingleLine(true);
        socketInput.setText(prefs.getString(KEY_SOCKET_PATH, DEFAULT_SOCKET_PATH));
        socketInput.setHint(DEFAULT_SOCKET_PATH);
        socketInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void afterTextChanged(Editable s) {
                getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                    .putString(KEY_SOCKET_PATH, s.toString().trim()).apply();
            }
        });
        root.addView(socketInput);

        // Connect with root
        Switch rootSwitch = new Switch(this);
        rootSwitch.setText("Connect with root (helper)");
        rootSwitch.setTextSize(14);
        rootSwitch.setPadding(0, dp(16), 0, 0);
        rootSwitch.setChecked(prefs.getBoolean(KEY_USE_ROOT, false));
        rootSwitch.setOnCheckedChangeListener((v, checked) ->
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                .putBoolean(KEY_USE_ROOT, checked).apply());
        root.addView(rootSwitch);

        TextView rootHint = new TextView(this);
        rootHint.setText("Launches a root helper (su) that connects to the socket "
            + "and passes the connection back to the app. Takes effect on next "
            + "connect (reopen the app).");
        rootHint.setTextSize(12);
        rootHint.setTextColor(Color.GRAY);
        rootHint.setPadding(0, dp(4), 0, 0);
        root.addView(rootHint);

        // Forward microphone: capture the device mic and expose it to the Linux
        // desktop as a recording source. Requires the RECORD_AUDIO permission, which
        // MainActivity requests when this is on.
        Switch micSwitch = new Switch(this);
        micSwitch.setText("Forward microphone to desktop");
        micSwitch.setTextSize(14);
        micSwitch.setPadding(0, dp(16), 0, 0);
        micSwitch.setChecked(prefs.getBoolean(KEY_MIC_ENABLED, false));
        micSwitch.setOnCheckedChangeListener((v, checked) ->
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                .putBoolean(KEY_MIC_ENABLED, checked).apply());
        root.addView(micSwitch);

        TextView micHint = new TextView(this);
        micHint.setText("Desktop audio always plays through this device's speaker. "
            + "Enabling this also sends the mic the other way. Takes effect on next "
            + "connect (reopen the app).");
        micHint.setTextSize(12);
        micHint.setTextColor(Color.GRAY);
        micHint.setPadding(0, dp(4), 0, 0);
        root.addView(micHint);

        // Forward camera: expose the device camera(s) to the Linux desktop. When on,
        // the app pre-creates the camera service resources at startup (CameraX is only
        // opened once the desktop actually requests a recording). Requires the CAMERA
        // permission, which MainActivity requests when this is enabled.
        Switch cameraSwitch = new Switch(this);
        cameraSwitch.setText("Forward camera to desktop");
        cameraSwitch.setTextSize(14);
        cameraSwitch.setPadding(0, dp(16), 0, 0);
        cameraSwitch.setChecked(prefs.getBoolean(KEY_CAMERA_ENABLED, false));
        cameraSwitch.setOnCheckedChangeListener((v, checked) ->
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                .putBoolean(KEY_CAMERA_ENABLED, checked).apply());
        root.addView(cameraSwitch);

        TextView cameraHint = new TextView(this);
        cameraHint.setText("Lets the desktop open this device's camera(s) as a video "
            + "source. The camera only turns on while the desktop is actively "
            + "recording. Takes effect on next return to the app; first use prompts "
            + "for the camera permission.");
        cameraHint.setTextSize(12);
        cameraHint.setTextColor(Color.GRAY);
        cameraHint.setPadding(0, dp(4), 0, 0);
        root.addView(cameraHint);

        // Audio latency presets, separately for the speaker (playback) and microphone
        // (capture) paths. The chosen buffer is forwarded to the producer's PipeWire
        // nodes; smaller = lower latency but more risk of audio glitches.
        TextView latTitle = new TextView(this);
        latTitle.setText("Audio latency");
        latTitle.setTextSize(15);
        latTitle.setTypeface(Typeface.DEFAULT_BOLD);
        latTitle.setPadding(0, dp(20), 0, 0);
        root.addView(latTitle);

        root.addView(makeLatencySpinner("Speaker (desktop → phone)",
                                        KEY_SPEAKER_LATENCY_MS, prefs));
        root.addView(makeLatencySpinner("Microphone (phone → desktop)",
                                        KEY_MIC_LATENCY_MS, prefs));

        TextView latHint = new TextView(this);
        latHint.setText("Lower presets cut latency but can cause crackles/dropouts on a "
            + "busy device. Applies when you return to the desktop view.");
        latHint.setTextSize(12);
        latHint.setTextColor(Color.GRAY);
        latHint.setPadding(0, dp(4), 0, 0);
        root.addView(latHint);
    }

    private void addResolutionSection(LinearLayout root) {
    SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
    
    TextView header = new TextView(this);
    header.setText("Display Resolution");
    header.setTextSize(16);
    header.setTypeface(null, Typeface.BOLD);
    header.setPadding(0, dp(32), 0, dp(8));
    root.addView(header);
    
    // Width / height fields. Created first (but added below the preset picker) so
    // the picker can populate them; their TextWatchers are the single source of
    // truth that persists custom_width/custom_height.
    final EditText widthInput = new EditText(this);
    widthInput.setSingleLine(true);
    widthInput.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
    widthInput.setHint("Width (e.g. 1920)");
    widthInput.setText(String.valueOf(prefs.getInt("custom_width", 0)));
    widthInput.addTextChangedListener(new TextWatcher() {
        public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
        public void onTextChanged(CharSequence s, int a, int b, int c) {}
        public void afterTextChanged(Editable s) {
            try {
                int w = Integer.parseInt(s.toString().trim());
                prefs.edit().putInt("custom_width", w).apply();
            } catch (NumberFormatException e) {}
        }
    });

    final EditText heightInput = new EditText(this);
    heightInput.setSingleLine(true);
    heightInput.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
    heightInput.setHint("Height (e.g. 1080)");
    heightInput.setText(String.valueOf(prefs.getInt("custom_height", 0)));
    heightInput.addTextChangedListener(new TextWatcher() {
        public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
        public void onTextChanged(CharSequence s, int a, int b, int c) {}
        public void afterTextChanged(Editable s) {
            try {
                int h = Integer.parseInt(s.toString().trim());
                prefs.edit().putInt("custom_height", h).apply();
            } catch (NumberFormatException e) {}
        }
    });

    // Preset picker: fills width/height (which persist via their watchers). Index
    // 0 is a no-op placeholder so the Spinner's initial auto-selection and manual
    // edits leave the fields untouched.
    Spinner presetSpinner = new Spinner(this);
    presetSpinner.setAdapter(new ArrayAdapter<>(this,
        android.R.layout.simple_spinner_dropdown_item, RES_PRESET_LABELS));
    presetSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
        @Override
        public void onItemSelected(AdapterView<?> parent, View v, int pos, long id) {
            int[] wh = resolvePreset(pos);
            if (wh == null) return;
            widthInput.setText(String.valueOf(wh[0]));
            heightInput.setText(String.valueOf(wh[1]));
        }
        @Override
        public void onNothingSelected(AdapterView<?> parent) {}
    });
    root.addView(presetSpinner);

    root.addView(widthInput);
    root.addView(heightInput);
    
    TextView hint = new TextView(this);
    hint.setText("Pick a preset or enter values manually. Leave 0 for native "
        + "resolution. \"Screen ×\" scales this device's panel (landscape). "
        + "Takes effect on next connect.");
    hint.setTextSize(12);
    hint.setTextColor(Color.GRAY);
    hint.setPadding(0, dp(4), 0, 0);
    root.addView(hint);
    }

    // Maps a RES_PRESET_LABELS index to {width, height}, or null for the index-0
    // placeholder. "Screen ×" presets are derived from the live panel size.
    private int[] resolvePreset(int pos) {
        switch (pos) {
            case 1: return new int[]{0, 0};
            case 2: return new int[]{3840, 2160};
            case 3: return new int[]{2560, 1440};
            case 4: return new int[]{1920, 1080};
            case 5: return new int[]{1280, 720};
            case 6: return new int[]{854, 480};
            case 7: return scaleScreen(1.0f);
            case 8: return scaleScreen(0.8f);
            case 9: return scaleScreen(0.75f);
            case 10: return scaleScreen(0.5f);
            case 11: return scaleScreen(0.25f);
            default: return null;
        }
    }

    // Scales the device panel by `f`, normalised to landscape (long side = width)
    // and rounded down to even dimensions, which compositors/encoders expect.
    private int[] scaleScreen(float f) {
        WindowManager wm = (WindowManager) getSystemService(WINDOW_SERVICE);
        Rect b = wm.getMaximumWindowMetrics().getBounds();
        int longSide = Math.max(b.width(), b.height());
        int shortSide = Math.min(b.width(), b.height());
        int w = Math.round(longSide * f) & ~1;
        int h = Math.round(shortSide * f) & ~1;
        return new int[]{w, h};
    }

    /* A labelled latency picker that persists the selected preset (ms) under `key`. */
    private View makeLatencySpinner(String label, final String key, SharedPreferences prefs) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(0, dp(12), 0, 0);

        TextView tv = new TextView(this);
        tv.setText(label);
        tv.setTextSize(14);
        box.addView(tv);

        Spinner sp = new Spinner(this);
        sp.setAdapter(new ArrayAdapter<>(this,
            android.R.layout.simple_spinner_dropdown_item, LATENCY_LABELS));

        int cur = prefs.getInt(key, 0);
        int idx = 0;
        for (int i = 0; i < LATENCY_MS.length; i++) {
            if (LATENCY_MS[i] == cur) { idx = i; break; }
        }
        sp.setSelection(idx);

        sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View v, int pos, long id) {
                getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                    .putInt(key, LATENCY_MS[pos]).apply();
            }
            @Override
            public void onNothingSelected(AdapterView<?> parent) {}
        });
        box.addView(sp);
        return box;
    }

    private void startListening() {
        if (isListening) return;
        isListening = true;
        bindButton.setText("Listening... (5s)");

        listenTimer = new CountDownTimer(5000, 1000) {
            @Override
            public void onTick(long millisUntilFinished) {
                bindButton.setText("Listening... (" + (millisUntilFinished / 1000) + "s)");
            }

            @Override
            public void onFinish() {
                finishListening(UNBOUND);
            }
        }.start();
    }

    private void finishListening(int keycode) {
        isListening = false;
        listenTimer.cancel();

        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        prefs.edit().putInt(KEY_BOUND_KEYCODE, keycode).apply();

        bindButton.setText("Bind Virtual Keyboard Key");
        updateStatus();
    }

    private void updateStatus() {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        int bound = prefs.getInt(KEY_BOUND_KEYCODE, UNBOUND);
        if (bound == UNBOUND) {
            statusText.setText("Current: None");
        } else {
            String name = KEY_NAMES.get(bound);
            if (name == null) name = "Keycode " + bound;
            statusText.setText("Current: " + name);
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (!isListening) return super.onKeyDown(keyCode, event);

        // Ignore generic Virtual Keyboard keycode (it's a placeholder)
        if (keyCode == KeyEvent.KEYCODE_UNKNOWN) return true;

        finishListening(keyCode);
        Log.i(TAG, "Bound keycode: " + keyCode);
        return true;
    }

    private int dp(int dp) {
        return (int) (dp * getResources().getDisplayMetrics().density);
    }
}
