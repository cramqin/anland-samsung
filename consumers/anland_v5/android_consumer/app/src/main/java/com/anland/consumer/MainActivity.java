package com.anland.consumer;

import android.Manifest;
import android.app.Activity;
import android.app.ActivityManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.graphics.Matrix;
import android.hardware.display.DisplayManager;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.FrameLayout;
import android.util.DisplayMetrics;   // ADDED

import java.nio.charset.StandardCharsets;


public class MainActivity extends Activity
        implements SurfaceHolder.Callback, SystemIME.Host {
    private static final String TAG = "Anland";

    private SurfaceView surfaceView;
    private boolean surfaceReady = false;
    // System-clipboard bridge; also the target for the native clipboard callbacks.
    private Clipboard clipboard;
    private static final String PREFS_NAME = "anland_settings";
    private int customScreenWidth = 0;
    private int customScreenHeight = 0;
    private int viewWidth = 0;
    private int viewHeight = 0;
    private static final String KEY_BOUND_KEYCODE = "bound_keycode";
    private static final String KEY_SOCKET_PATH = "socket_path";
    private static final String KEY_USE_ROOT = "use_root";
    private static final String KEY_MIC_ENABLED = "mic_enabled";
    private static final String KEY_CAMERA_ENABLED = "camera_enabled";
    // Latency presets in ms; 0 = engine default. Shared with SettingsActivity.
    static final String KEY_SPEAKER_LATENCY_MS = "speaker_latency_ms";
    static final String KEY_MIC_LATENCY_MS = "mic_latency_ms";
    private static final int REQ_RECORD_AUDIO = 1001;
    private static final int REQ_CAMERA = 1002;
    // Camera service fds/threads are created once and persist across reconnects;
    // this guards that one-time init (see applyCameraState).
    private boolean cameraInited = false;
    private static final String DEFAULT_SOCKET_PATH = "/data/local/tmp/display_daemon.sock";
    // Multi-instance launch parameters. A secondary window is started with these
    // Intent extras (see SecondaryActivity / SettingsActivity); the launcher icon
    // starts MainActivity with none, i.e. the default socket and window name "anland".
    static final String EXTRA_SOCKET_PATH = "socket_path";
    static final String EXTRA_WINDOW_NAME = "window_name";
    // This window's own native transport instance (its own consumer_state handle).
    private Native mNative;
    // Socket path from the launch Intent; overrides the saved pref when non-null.
    private String mSocketOverride = null;
    // Title shown in recents / freeform (setTaskDescription); default "anland".
    private String mWindowName = "anland";
    // Live windows keyed by their resolved socket path, so a launch that targets a
    // socket already on screen can focus that window instead of opening a duplicate.
    // Only touched on the main thread (onCreate / onResume / onDestroy).
    private static final java.util.Map<String, MainActivity> sWindowsBySocket =
            new java.util.HashMap<>();
    // The socket path this window is currently registered under in sWindowsBySocket.
    private String mRegisteredSocket = null;
    // Set when onCreate found the target socket missing and bounced to Settings
    // (no pipeline was ever initialized). Makes onPause/onResume no-op-and-exit.
    private boolean mForceSettings = false;
    private static final String KEY_ACCESSIBILITY_ENABLED = "accessibility_key_intercept";
    private static final String KEY_EXTRA_KEYS_MODE = "extra_keys_mode";
    private static final String KEY_BACK_OPENS_EXTRA_KEYS = "back_opens_extra_keys";
    private static final String KEY_EXTRA_KEYS_LAYOUT = "extra_keys_layout";
    // Linux input-event-codes.h: KEY_BACK (the browser-back key).
    private static final int EVDEV_BROWSER_BACK = 158;
    // When on, the IME and extra-keys bar float over the display instead of
    // shrinking it: the bar rides up with the keyboard but the surface keeps
    // its full size. See relayout() and buildExtraKeysBar().
    private static final String KEY_KEYBOARD_FLOATING = "keyboard_floating";
    private boolean mKeyboardFloating = false;
    // Persistent "tap to open Settings" notification, toggleable in Settings > General.
    private static final String KEY_NOTIFICATION_ENABLED = "settings_notification";
    private static final String KEY_AUTO_STRETCH = "auto_stretch";
    private boolean autoStretch = true;
    private float surfaceOffsetX = 0f;
    private float surfaceOffsetY = 0f;
    private float surfaceScale = 1f;
    // System soft-keyboard bridge: hidden input, text forwarding and toggle.
    private SystemIME systemIme;
    private int mImeBottom = 0;   // last IME bottom inset
    private int mBarHeight = 0;   // extra-keys bar height in px
    private ExtraKeysBar extraKeysBar;
    private FrameLayout mRoot;    // content root, host of the extra-keys bar
    private float mDensity = 1f;
    // Layout JSON the current bar was built from; used to detect edits on resume.
    private String mAppliedLayoutJson = "";

    public static MainActivity sInstance;

    // ADDED: VirtualKeyboardView instance
    private VirtualKeyboardView virtualKeyboardView;

    // ==================== 触摸板相关设置 ====================
    public static final String KEY_TOUCHPAD_MODE = "touchpad_mode";
    public static final String KEY_MOUSE_ACCEL = "mouse_speed"; // 名称仍为 speed，实际控制加速度强度
    // Capture an external mouse/touchpad as a relative pointer so it cannot reach
    // the Android screen edges. This is deliberately opt-in: existing installations
    // keep the old absolute-pointer behaviour until the user enables it.
    public static final String KEY_POINTER_CAPTURE = "pointer_capture";
    // Immersive full-input capture: a root helper (libinputgrab.so) takes an exclusive
    // EVIOCGRAB on the touchscreen + key devices, so Android sees no input at all
    // (home/back/recents gestures, notification shade, and every key are dead); the raw
    // evdev stream is forwarded to Linux instead.
    //
    // This setting only ARMS the feature; it never immerses by itself. Immersion is
    // always entered and left by the user pressing the key bound in Settings, and
    // every launch/resume starts un-immersed. Nothing grabs input without an explicit
    // press, which is what keeps a bad state from ever locking the device down.
    public static final String KEY_FULL_KEY_CAPTURE = "full_key_capture";
    private boolean fullKeyCaptureEnabled = false;

    // The single key bound in Settings that toggles immersion. Immersion cannot be
    // entered without one: the helper detects that same key root-side to leave, so a
    // binding is what guarantees there is always a way out.
    public static final String KEY_IMMERSION_KEYCODE = "immersion_keycode";
    // Raw evdev scancode of that key, captured from the KeyEvent when it was bound.
    // The helper matches on this, and taking it from the event works for keys
    // KeyCodeMapper has no entry for -- the volume keys among them.
    public static final String KEY_IMMERSION_SCANCODE = "immersion_scancode";
    private static final int UNBOUND = -1;
    private int immersionKeycode = UNBOUND;
    private int immersionScancode = 0;
    private boolean immersed = false;
    // Tracked from onWindowFocusChanged rather than read via hasWindowFocus(), so the
    // grab decision does not depend on when the framework updates the decor view.
    private boolean mHasWindowFocus = false;
    // Grab start/stop must never run on the main thread: they fork `su` and join the
    // grab thread, and su can take seconds to grant -- that blocked main thread is
    // what produced a real ANR ("Waited 5000ms for PointerCaptureChangedEvent").
    // A single worker keeps start/stop strictly ordered while the UI stays responsive.
    private final java.util.concurrent.ExecutorService grabExecutor =
            java.util.concurrent.Executors.newSingleThreadExecutor();

    // Routing gate: when on, non-mouse touches go to the virtual touchpad.
    private boolean isTouchpadMode = true;
    // Finger-gesture touchpad (relative motion, taps, drag, two-finger scroll).
    private VirtualTouchpad virtualTouchpad;
    // Reuses the original VirtualTouchpad gesture state machine for raw
    // SOURCE_TOUCHPAD events. Its movement output is intentionally ignored;
    // raw relative axes still go through the capture-specific cursor adapter.
    private VirtualTouchpad capturedTouchpadRecognizer;

    // Pointer-capture state. Android delivers captured mouse/touchpad events
    // directly to the view hierarchy, bypassing
    // Activity.onGenericMotionEvent/onTouchEvent.  Keep a virtual absolute cursor
    // for the compositor protocol, which requires both an absolute position and a
    // relative delta for every motion event.
    private boolean pointerCaptureEnabled = false;
    private boolean pointerCaptureSuppressed = false;
    // Tracks the Back key whose DOWN released pointer capture, so only its matching
    // UP is swallowed. Device/downTime matching prevents a stale missing UP from
    // consuming a later, unrelated Back press.
    private boolean pointerCaptureBackUpPending = false;
    private boolean pointerCaptureBackWildcard = false;
    private int pointerCaptureBackDeviceId = 0;
    private long pointerCaptureBackDownTime = 0L;
    private float pointerX = Float.NaN;
    private float pointerY = Float.NaN;

    // Raw hardware-touchpad coordinate state while pointer capture is active.
    // Gesture recognition itself is shared with VirtualTouchpad above.
    private float capturedTouchpadAccel = 1.0f;
    private boolean capturedTouchpadBaselineValid = false;
    private int capturedTouchpadBaselinePointers = 0;
    private float capturedTouchpadLastCentroidX = 0f;
    private float capturedTouchpadLastCentroidY = 0f;
    private final float[] capturedTouchpadResolvedDelta = new float[2];
    private final Matrix capturedTouchpadTransform = new Matrix();

    static {
        // Loads the single shared .so backing MainActivity, Native and
        // CameraServices; the last two only declare their natives.
        System.loadLibrary("anland_consumer");
    }
    // Forwards the current display refresh rate to the daemon so KWin can repace
    // its RenderLoop. Re-fires on every onDisplayChanged (e.g. 60/90/120 switch).
    private final DisplayManager.DisplayListener displayListener =
        new DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int displayId) {}
            @Override public void onDisplayRemoved(int displayId) {}
            @Override public void onDisplayChanged(int displayId) {
                Display d = getDisplay();
                if (d != null && d.getDisplayId() == displayId) {
                    pushRefreshRate();
                    // Keep the immersive-grab touch transform aligned with the
                    // current rotation (the activity handles orientation config
                    // changes itself, so it is not recreated on rotate).
                    if (mNative != null)
                        mNative.setGrabRotation(d.getRotation());
                }
            }
        };

    // Called from native on_fallback (display lib dropped the connection). Runs on a
    // native worker thread, so hop to the UI thread before touching the toast/finish.
    // If the daemon socket is gone the daemon really went down, so close this window.
    public void onFallback(){
        runOnUiThread(() -> {
            if (!isSocketFile(resolveSocketPath())) {
                //exit
                android.widget.Toast.makeText(this, "Deamon Down",
                        android.widget.Toast.LENGTH_SHORT).show();
                finish();
            }
        });
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        mHasWindowFocus = hasFocus;
        // Release FIRST, before the root socket probe below: that probe forks `su` and
        // must never be able to delay handing input back to Android.
        if (!hasFocus)
            stopImmersiveGrab();
        if (!isSocketFile(resolveSocketPath())) {
            //exit
            android.widget.Toast.makeText(this, "Deamon Down",
                    android.widget.Toast.LENGTH_SHORT).show();
            finish();
        }
        if (hasFocus) {
            // Become the accessibility-key target and the focused instance, so real
            // camera frames route to this window (others get blank frames).
            sInstance = this;
            if (mNative != null) mNative.setFocused(true);
        }
        if (hasFocus && clipboard != null) {
            clipboard.pushClipboard();
        }
        if (mRoot != null) {
            if (hasFocus)
                mRoot.post(this::syncPointerCapture);
            else {
                clearPointerCaptureBackTracking();
                releasePointerCapture(false);
            }
        }
        // Immersion never survives losing focus: the grab must not outlive our
        // foreground, or the device would be input-dead behind us. Coming back starts
        // un-immersed, so the user re-enters deliberately with the toggle key.
        // The release itself already happened at the top of this method.
        if (!hasFocus)
            immersed = false;
    }

    // Enter immersion: launch the root helper, which grabs the touchscreen + keys.
    // Only ever called from the toggle key, never from a lifecycle callback, so input
    // is never taken without an explicit press.
    private void enterImmersion() {
        if (mNative == null || !fullKeyCaptureEnabled || immersed || !mHasWindowFocus)
            return;
        // The helper reads raw evdev, so it needs the scancode, not the Android
        // keycode. Prefer the one captured when the key was bound; fall back to the
        // mapping table for bindings made before that was recorded. If neither
        // resolves, the helper would have no way to detect the exit key -- so refuse
        // to grab at all rather than take over with no way out.
        int exitScan = immersionScancode > 0
                ? immersionScancode : KeyCodeMapper.getScanCode(immersionKeycode);
        if (exitScan <= 0) {
            android.widget.Toast.makeText(this, R.string.immerse_key_unusable,
                    android.widget.Toast.LENGTH_LONG).show();
            return;
        }
        immersed = true;
        android.widget.Toast.makeText(this, R.string.immerse_entered,
                android.widget.Toast.LENGTH_SHORT).show();
        String helper = getApplicationInfo().nativeLibraryDir + "/libinputgrab.so";
        String bridge = getCacheDir().getAbsolutePath() + "/anland_grab.sock";
        int rotation = currentDisplayRotation();
        final Native n = mNative;
        postGrabTask(() -> n.startInputGrab(helper, bridge, rotation, exitScan));
    }

    private void stopImmersiveGrab() {
        if (mNative == null)
            return;
        final Native n = mNative;
        postGrabTask(n::stopInputGrab);
    }

    // The executor is shut down in onDestroy, and a late lifecycle callback can still
    // reach here; a rejected task means the pipeline is already being torn down (which
    // releases the grab natively), so dropping it is correct rather than crashing.
    private void postGrabTask(Runnable task) {
        try {
            grabExecutor.execute(task);
        } catch (java.util.concurrent.RejectedExecutionException e) {
            Log.w(TAG, "grab task rejected (shutting down)");
        }
    }

    // Surface.ROTATION_* (0..3). Raw evdev touch is in the panel's natural
    // orientation, so native rotates it into surface space by this value.
    private int currentDisplayRotation() {
        Display d = getDisplay();
        return d != null ? d.getRotation() : Surface.ROTATION_0;
    }

    // Called from native (grab thread) once the helper has released the grab on its
    // own -- the user pressed the toggle key, it gave up on a stalled app, or it died.
    // So this only has to catch up our own state. Posted to the UI thread (the grab
    // thread must not self-join). Not the only way out: even if this never runs, the
    // helper has already ungrabbed, so input is back with Android regardless.
    public void onImmersionReleased() {
        runOnUiThread(this::exitImmersion);
    }

    private void exitImmersion() {
        if (immersed)
            android.widget.Toast.makeText(this, R.string.immerse_exited,
                    android.widget.Toast.LENGTH_SHORT).show();
        immersed = false;
        stopImmersiveGrab();
    }

    // The bound key toggles immersion. Entering is detected here from an ordinary
    // Android key event (we are not grabbing yet); leaving is detected by the root
    // helper, which sees the same key and releases the grab itself -- so the way out
    // never depends on this process being responsive.
    //
    // Reaching this while `immersed` means the grab is NOT actually in force (the
    // helper failed to start, e.g. root was denied, or it already died), because
    // otherwise Android would not be delivering us keys -- so treat it as an exit and
    // the user can never be stuck unable to toggle either way.
    // Returns true if the event was consumed.
    private boolean handleImmersionKey(KeyEvent event) {
        if (!fullKeyCaptureEnabled || immersionKeycode == UNBOUND)
            return false;
        if (event.getKeyCode() != immersionKeycode)
            return false;
        // Swallow both DOWN and UP: this key is the toggle, so Linux must not also see
        // it (the helper likewise never forwards it while immersed). The repeatCount
        // guard stops a held key from toggling over and over.
        if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
            if (immersed) exitImmersion();
            else          enterImmersion();
        }
        return true;
    }

    private void pushRefreshRate() {
        Display d = getDisplay();
        if (d != null)
            mNative.setRefreshRate(d.getRefreshRate());
    }

    // Push the current connection settings (socket path / root mode) to native
    // before (re)connecting. The root helper is the executable bundled in the
    // app's native lib dir; the bridge is a unix socket in our cache dir that
    // the helper, launched via su, uses to hand back the daemon fd.
    private void applyConnectionConfig() {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        String sock = resolveSocketPath();
        boolean useRoot = prefs.getBoolean(KEY_USE_ROOT, true);
        String helperPath = getApplicationInfo().nativeLibraryDir + "/libfdhelper.so";
        String bridgePath = getCacheDir().getAbsolutePath() + "/anland_fdbridge.sock";
        mNative.configure(sock, useRoot, helperPath, bridgePath);
        int customW = prefs.getInt("custom_width", 0);
        int customH = prefs.getInt("custom_height", 0);
        customScreenWidth = prefs.getInt("custom_width", 0);
        customScreenHeight = prefs.getInt("custom_height", 0);
        mNative.setCustomResolution(customW, customH);
    }

    // The daemon socket this window targets: the launch-Intent override if any,
    // else the saved pref, else the built-in default. Never null/blank. This is
    // both the native connection target and this window's dedup key.
    private String resolveSocketPath() {
        String sock = mSocketOverride;
        if (sock == null || sock.trim().isEmpty())
            sock = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                    .getString(KEY_SOCKET_PATH, DEFAULT_SOCKET_PATH);
        if (sock == null || sock.trim().isEmpty())
            sock = DEFAULT_SOCKET_PATH;
        return sock.trim();
    }

    // Start (or restart) this window's native pipeline, but only if the daemon
    // socket is still a live socket. The daemon can go down after launch, so
    // re-check on every (re)connect; if it is gone, report it and exit the window.
    private void startNative(android.view.Surface surface) {
        if (!isSocketFile(resolveSocketPath())) {
            android.widget.Toast.makeText(this, "Deamon Down",
                    android.widget.Toast.LENGTH_SHORT).show();
            finish();
            return;
        }
        mNative.start(surface, clipboard, this);
    }

    // True only when `path` exists and is a unix-domain socket. In root mode the
    // daemon socket usually lives in a root-only location (e.g. /data/local/tmp),
    // which this untrusted_app process cannot stat() directly -- a direct stat
    // would EACCES and wrongly report "no socket". So when root mode is on we run
    // the bundled helper as root (`su -c "<helper> <path> test"`) and read its
    // exit code instead; otherwise we stat() locally.
    private boolean isSocketFile(String path) {
        boolean useRoot = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .getBoolean(KEY_USE_ROOT, true);
        return useRoot ? isSocketFileRoot(path) : isSocketFileLocal(path);
    }

    // stat(2) both resolves existence and reports the file type, so a stale
    // regular file / dir (or an unreadable / missing path -> ErrnoException)
    // counts as "no socket".
    private static boolean isSocketFileLocal(String path) {
        try {
            android.system.StructStat st = android.system.Os.stat(path);
            return android.system.OsConstants.S_ISSOCK(st.st_mode);
        } catch (android.system.ErrnoException e) {
            return false;
        }
    }

    // Probe the socket from root context via the bundled helper's "test" mode.
    // Exit 0 means the path exists and is a unix socket; anything else (including
    // su being unavailable / denied, which throws) counts as "no socket".
    private boolean isSocketFileRoot(String path) {
        String helperPath = getApplicationInfo().nativeLibraryDir + "/libfdhelper.so";
        Process p = null;
        try {
            p = new ProcessBuilder("su", "-c", helperPath + " " + path + " test")
                    .redirectErrorStream(true)
                    .start();
            // Bounded: this runs on the main thread (onWindowFocusChanged, onCreate,
            // startNative) and `su` can sit on an unanswered root prompt indefinitely,
            // which is exactly the main-thread block that ANRs us. A timeout is
            // inconclusive, not proof the daemon is gone -- returning false here would
            // toast "Deamon Down" and finish(). A genuinely dead daemon is still caught
            // by the native on_fallback path.
            if (!p.waitFor(1500, java.util.concurrent.TimeUnit.MILLISECONDS)) {
                Log.w(TAG, "root socket probe timed out; assuming daemon alive");
                return true;
            }
            return p.exitValue() == 0;
        } catch (Exception e) {
            Log.w(TAG, "root socket probe failed: " + e);
            return false;
        } finally {
            if (p != null) p.destroy();
        }
    }

    // (Re)register this window under its current socket in sWindowsBySocket. A
    // window with no Intent override resolves its socket from the saved pref, which
    // the user can change in Settings, so re-key whenever it may have moved.
    private void registerWindow() {
        String sock = resolveSocketPath();
        if (sock.equals(mRegisteredSocket)) return;
        if (mRegisteredSocket != null)
            sWindowsBySocket.remove(mRegisteredSocket, this);
        sWindowsBySocket.put(sock, this);
        mRegisteredSocket = sock;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        applyOrientation();

        // Apply the launch parameters: socket path (overrides the saved pref) and
        // window name (task title). Read them before anything else so the dedup
        // check below sees this window's target socket.
        Intent launch = getIntent();
        if (launch != null) {
            String sock = launch.getStringExtra(EXTRA_SOCKET_PATH);
            if (sock != null && !sock.trim().isEmpty())
                mSocketOverride = sock.trim();
            String name = launch.getStringExtra(EXTRA_WINDOW_NAME);
            if (name != null && !name.trim().isEmpty())
                mWindowName = name.trim();
        }

        // Skip opening a duplicate: if another live window already targets this
        // socket, bring it to the front and drop this (freshly spawned) task.
        MainActivity existing = sWindowsBySocket.get(resolveSocketPath());
        if (existing != null && existing != this && !existing.isFinishing()) {
            ActivityManager am = getSystemService(ActivityManager.class);
            if (am != null) am.moveTaskToFront(existing.getTaskId(), 0);
            finishAndRemoveTask();
            return;
        }

        // The target must exist AND be a unix-domain socket before we bring up any
        // pipeline. If it is not: a parameter launch has nowhere to fall back to
        // (toast and quit); a plain launcher start bounces to Settings so the user
        // can fix the path.
        if (!isSocketFile(resolveSocketPath())) {
            if (mSocketOverride != null) {
                android.widget.Toast.makeText(this, "Socket Not Found",
                        android.widget.Toast.LENGTH_SHORT).show();
                finishAndRemoveTask();
                return;
            }
            mForceSettings = true;
            startActivity(new Intent(this, SettingsActivity.class));
            return;
        }

        sInstance = this;

        // Each window owns its own native pipeline.
        mNative = new Native();
        setTaskDescription(new ActivityManager.TaskDescription(mWindowName));

        clipboard = new Clipboard(this, mNative);

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN);
        // Take over inset handling: the IME insets are dispatched to our
        // OnApplyWindowInsetsListener (so we can resize the surface) instead of
        // the system auto-panning the fullscreen window.
        getWindow().setDecorFitsSystemWindows(false);

        surfaceView = new SurfaceView(this);
        // Give the content view an explicit focus target. Pointer-captured events
        // are routed along the focused-view path; the root override below then
        // intercepts them before the focused child handles them.
        surfaceView.setFocusable(true);
        surfaceView.setFocusableInTouchMode(true);
        systemIme = new SystemIME(this, this, mNative);

        // Pointer-captured mouse/touchpad events are dispatched by ViewRootImpl to the
        // focused view hierarchy, not to Activity.onGenericMotionEvent(). Intercept
        // them at the root before they are routed to the hidden IME or any overlay;
        // this keeps capture working even while the soft keyboard owns focus.
        FrameLayout root = new FrameLayout(this) {
            @Override
            public boolean dispatchCapturedPointerEvent(MotionEvent event) {
                if (handleCapturedPointerEvent(event))
                    return true;
                return super.dispatchCapturedPointerEvent(event);
            }

            @Override
            public void dispatchPointerCaptureChanged(boolean hasCapture) {
                super.dispatchPointerCaptureChanged(hasCapture);
                if (!hasCapture) {
                    releaseAllMouseButtons();
                    resetCapturedTouchpadGesture();
                }
            }
        };
        root.addView(surfaceView, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT));
        // 1x1 so the IME target never overlaps the surface and steals touches.
        root.addView(systemIme.getInputView(), new FrameLayout.LayoutParams(1, 1));

        // Bottom extra-keys bar (Termux-style). Hidden by default; toggled by the
        // settings switch and synced in onResume. The layout (and thus the row
        // count / height) comes from the user's JSON config; see buildExtraKeysBar.
        mRoot = root;
        mDensity = getResources().getDisplayMetrics().density;
        mKeyboardFloating = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .getBoolean(KEY_KEYBOARD_FLOATING, true);
        buildExtraKeysBar();

        // ADDED: Create VirtualKeyboardView (hidden initially)
        virtualKeyboardView = new VirtualKeyboardView(this);
        virtualKeyboardView.setVisibility(View.GONE);
        virtualKeyboardView.setOnKeyEventListener(new VirtualKeyboardView.OnKeyEventListener() {
            @Override
            public void onKeyDown(int scanCode) {
                mNative.sendKey(0, scanCode);
            }
            @Override
            public void onKeyUp(int scanCode) {
                mNative.sendKey(1, scanCode);
            }
        });
        // Add to root with no gravity – we will position manually.
        root.addView(virtualKeyboardView, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.NO_GRAVITY
        ));
        // Reposition the virtual keyboard when the root layout size changes
        // (e.g. freeform / small-window mode resize).
        root.addOnLayoutChangeListener((v, left, top, right, bottom,
                oldLeft, oldTop, oldRight, oldBottom) -> {
            int newW = right - left;
            int newH = bottom - top;
            int oldW = oldRight - oldLeft;
            int oldH = oldBottom - oldTop;
            Log.d("VirtualKeyboard", "root layout changed: " + newW + "x" + newH
                    + " (was " + oldW + "x" + oldH + ")");
            if (newW != oldW || newH != oldH) {
                if (virtualKeyboardView != null
                        && virtualKeyboardView.getVisibility() == View.VISIBLE) {
                    positionVirtualKeyboard();
                }
            }
        });
        // Positioning happens lazily the first time the keyboard is shown
        // (see toggleVirtualKeyboard). Positioning it here would spin forever:
        // the view starts GONE and a GONE view is never measured.

        setContentView(root);
        // Establish a focused descendant after attachment so the DecorView routes
        // captured-pointer events through our content root even when the extra-keys
        // bar is hidden.
        surfaceView.requestFocus();
        surfaceView.getHolder().addCallback(this);

        root.setOnApplyWindowInsetsListener((v, insets) -> {
            // When the IME hides by any means (toggle, system back, or the IME's
            // own close button), release the hidden input so its focus state
            // stays in sync — otherwise reopening needs a second press.
            if (!insets.isVisible(WindowInsets.Type.ime())) {
                View focused = getCurrentFocus();
                systemIme.releaseHiddenInput();
                if (focused == systemIme.getInputView() || getCurrentFocus() == null)
                    surfaceView.requestFocus();
            }
            applyImeInset(insets);
            return v.onApplyWindowInsets(insets);
        });

        setupFullscreen();
        setupCursorHiding();

        // ===== 加载触摸板设置 =====
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        isTouchpadMode = prefs.getBoolean(KEY_TOUCHPAD_MODE, false);
        virtualTouchpad = new VirtualTouchpad(this, mNative);
        capturedTouchpadRecognizer = new VirtualTouchpad(this, mNative,
                new VirtualTouchpad.Output() {
                    @Override
                    public void onMotion(float dx, float dy) {
                        // Cursor motion is handled from AXIS_RELATIVE_X/Y below.
                        // Keeping this callback empty lets the original class
                        // remain the single source of truth for tap/drag/scroll
                        // state without duplicating its movement output.
                    }

                    @Override
                    public void onScroll(int axis, float value) {
                        if (mNative != null)
                            mNative.sendMouseScroll(axis, value);
                    }

                    @Override
                    public void onButton(int button, boolean pressed) {
                        if (mNative != null)
                            mNative.sendMouseButton(button, pressed);
                    }
                });
        capturedTouchpadAccel = Math.max(0.5f,
                Math.min(10.0f, prefs.getFloat(KEY_MOUSE_ACCEL, 1.0f)));
        virtualTouchpad.setAccelStrength(capturedTouchpadAccel);
        pointerCaptureEnabled = prefs.getBoolean(KEY_POINTER_CAPTURE, false);

        // Requesting capture before the window is attached is a no-op. The post
        // below covers the initial attach; onWindowFocusChanged() retries after a
        // focus transition (including returning from Settings).
        root.post(this::syncPointerCapture);

        registerWindow();
    }

    private static final String NOTIFICATION_CHANNEL = "anland_channel";
    private static final int NOTIFICATION_ID = 1;

    private void showSettingsNotification() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm == null) return;

        NotificationChannel channel = new NotificationChannel(
                NOTIFICATION_CHANNEL, getString(R.string.notification_channel_name),
                NotificationManager.IMPORTANCE_LOW);
        channel.setDescription(getString(R.string.notification_channel_desc));
        channel.setLockscreenVisibility(Notification.VISIBILITY_PUBLIC);
        nm.createNotificationChannel(channel);

        Intent intent = new Intent(this, SettingsActivity.class);
        intent.setAction(Intent.ACTION_MAIN);
        PendingIntent pi = PendingIntent.getActivity(this, 0, intent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Notification notification = new Notification.Builder(this, NOTIFICATION_CHANNEL)
                .setContentTitle(getString(R.string.notification_title))
                .setContentText(getString(R.string.notification_text))
                .setSmallIcon(R.mipmap.ic_launcher)
                .setContentIntent(pi)
                .setOngoing(true)
                .setShowWhen(false)
                .build();

        nm.notify(NOTIFICATION_ID, notification);
    }

    // ADDED: Helper to position virtual keyboard at bottom-center
    private void positionVirtualKeyboard() {
        if (virtualKeyboardView == null) return;
        int w = virtualKeyboardView.getMeasuredWidth();
        int h = virtualKeyboardView.getMeasuredHeight();
        if (w <= 0 || h <= 0) {
            // Only retry while the keyboard is actually visible. A GONE view is
            // never measured (width/height stay 0), so reposting unconditionally
            // would re-queue this Runnable on the main thread every frame forever
            // and cause global jank/卡顿 even while the keyboard is hidden.
            if (virtualKeyboardView.getVisibility() == View.VISIBLE) {
                virtualKeyboardView.post(this::positionVirtualKeyboard);
            }
            return;
        }
        // Use the root layout's dimensions instead of DisplayMetrics so that
        // positioning is correct in freeform / small-window mode.
        int parentW = mRoot.getWidth();
        int parentH = mRoot.getHeight();
        if (parentW <= 0 || parentH <= 0) {
            // Root not laid out yet — retry next frame.
            if (virtualKeyboardView.getVisibility() == View.VISIBLE) {
                virtualKeyboardView.post(this::positionVirtualKeyboard);
            }
            return;
        }
        float x = (parentW - w) / 2f;
        float y = parentH - h - dpToPx(50);
        // Clamp to visible area.
        x = Math.max(0, Math.min(x, parentW - w));
        y = Math.max(0, Math.min(y, parentH - h));
        virtualKeyboardView.setX(x);
        virtualKeyboardView.setY(y);
        Log.d("VirtualKeyboard", "positionVirtualKeyboard: x=" + x + ", y=" + y
                + " parent=" + parentW + "x" + parentH + " view=" + w + "x" + h);
    }

    private int dpToPx(int dp) {
        return (int) (dp * getResources().getDisplayMetrics().density);
    }

    private void setupFullscreen() {
        WindowInsetsController ctrl = getWindow().getInsetsController();
        if (ctrl != null) {
            ctrl.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
            ctrl.setSystemBarsBehavior(
                WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
        getWindow().getAttributes().layoutInDisplayCutoutMode =
            WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
    }

    private void setupCursorHiding() {
        surfaceView.setPointerIcon(PointerIcon.getSystemIcon(this, PointerIcon.TYPE_NULL));
    }

    /** Keep the window's pointer-capture state in sync with the saved setting. */
    private void syncPointerCapture() {
        if (mRoot == null)
            return;

        boolean shouldCapture = pointerCaptureEnabled
                && !pointerCaptureSuppressed
                && mRoot.hasWindowFocus();
        if (shouldCapture) {
            if (!mRoot.hasPointerCapture()) {
                // The request is window-wide.  Calling it on the root is safe even
                // when the hidden IME currently owns focus; the root override above
                // intercepts the resulting captured events first.
                mRoot.requestPointerCapture();
            }
        } else if (mRoot.hasPointerCapture()) {
            mRoot.releasePointerCapture();
        }
    }

    /** Release capture, optionally keeping it off until the next pointer click. */
    private void releasePointerCapture(boolean suppressUntilClick) {
        if (suppressUntilClick)
            pointerCaptureSuppressed = true;
        releaseAllMouseButtons();
        resetCapturedTouchpadGesture();
        if (mRoot != null && mRoot.hasPointerCapture())
            mRoot.releasePointerCapture();
    }

    private void clearPointerCaptureBackTracking() {
        pointerCaptureBackUpPending = false;
        pointerCaptureBackWildcard = false;
        pointerCaptureBackDeviceId = 0;
        pointerCaptureBackDownTime = 0L;
    }

    private void trackPointerCaptureBack(KeyEvent event) {
        pointerCaptureBackUpPending = true;
        pointerCaptureBackWildcard = event == null;
        if (event != null) {
            pointerCaptureBackDeviceId = event.getDeviceId();
            pointerCaptureBackDownTime = event.getDownTime();
        }
    }

    private boolean matchesTrackedPointerCaptureBack(KeyEvent event) {
        return pointerCaptureBackUpPending
                && (pointerCaptureBackWildcard
                    || (pointerCaptureBackDeviceId == event.getDeviceId()
                        && pointerCaptureBackDownTime == event.getDownTime()));
    }

    /**
     * Release capture for a non-mouse Android Back key and consume exactly the
     * matching DOWN/UP pair. Shared by the normal Activity and accessibility-key
     * paths so the setting behaves identically with interception enabled.
     */
    private boolean handlePointerCaptureBackKey(KeyEvent event) {
        if (event.getKeyCode() != KeyEvent.KEYCODE_BACK || isMouseKeyEvent(event))
            return false;

        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            if (event.getRepeatCount() > 0)
                return matchesTrackedPointerCaptureBack(event);

            // A fresh DOWN invalidates any pending state left by an OEM that did
            // not deliver the previous UP.
            clearPointerCaptureBackTracking();
            if (mRoot != null && mRoot.hasPointerCapture()) {
                releasePointerCapture(true);
                trackPointerCaptureBack(event);
                showPointerCaptureReleasedToast();
                return true;
            }
            return false;
        }

        if (event.getAction() == KeyEvent.ACTION_UP
                && matchesTrackedPointerCaptureBack(event)) {
            clearPointerCaptureBackTracking();
            return true;
        }
        return false;
    }

    private void showPointerCaptureReleasedToast() {
        android.widget.Toast.makeText(this, R.string.pointer_capture_released,
                android.widget.Toast.LENGTH_SHORT).show();
    }

    private void requestPointerCaptureAfterClick() {
        if (!pointerCaptureEnabled || mRoot == null)
            return;
        pointerCaptureSuppressed = false;
        mRoot.post(this::syncPointerCapture);
    }

    private int pointerViewWidth() {
        if (viewWidth > 0)
            return viewWidth;
        if (surfaceView != null && surfaceView.getWidth() > 0)
            return surfaceView.getWidth();
        return mRoot != null ? mRoot.getWidth() : 0;
    }

    private int pointerViewHeight() {
        if (viewHeight > 0)
            return viewHeight;
        if (surfaceView != null && surfaceView.getHeight() > 0)
            return surfaceView.getHeight();
        return mRoot != null ? mRoot.getHeight() : 0;
    }

    // pointerX/pointerY live in root-view coordinates, the same space MotionEvent
    // reports. With auto-stretch the surface fills the root, so that space starts at
    // 0; letterboxed, it starts at the surface's centering offset.
    private float pointerOriginX() {
        return autoStretch ? 0f : surfaceOffsetX;
    }

    private float pointerOriginY() {
        return autoStretch ? 0f : surfaceOffsetY;
    }

    private void ensurePointerPosition() {
        int width = pointerViewWidth();
        int height = pointerViewHeight();
        if (width <= 0 || height <= 0)
            return;
        float originX = pointerOriginX();
        float originY = pointerOriginY();
        if (!Float.isFinite(pointerX))
            pointerX = originX + width / 2f;
        if (!Float.isFinite(pointerY))
            pointerY = originY + height / 2f;
        pointerX = Math.max(originX, Math.min(pointerX, originX + width));
        pointerY = Math.max(originY, Math.min(pointerY, originY + height));
    }

    private float pointerScaleX() {
        return (customScreenWidth > 0 && pointerViewWidth() > 0)
                ? (float) customScreenWidth / pointerViewWidth() : 1.0f;
    }

    private float pointerScaleY() {
        return (customScreenHeight > 0 && pointerViewHeight() > 0)
                ? (float) customScreenHeight / pointerViewHeight() : 1.0f;
    }

    /** Handle mouse and hardware-touchpad events delivered through pointer capture. */
    private boolean handleCapturedPointerEvent(MotionEvent event) {
        // A few OEMs combine source capability bits. Prefer the touchpad path
        // whenever SOURCE_TOUCHPAD is present so raw multi-pointer events still
        // reach the gesture state machine.
        boolean touchpad = event.isFromSource(InputDevice.SOURCE_TOUCHPAD);
        boolean relativeMouse = !touchpad
                && event.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE);
        if (!relativeMouse && !touchpad)
            return false;
        if (mNative == null)
            return true;

        if (touchpad)
            return handleCapturedTouchpadEvent(event);

        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_MOVE || action == MotionEvent.ACTION_HOVER_MOVE) {
            // Relative mouse samples can be batched.  Consume every historical
            // sample so fast movements do not lose deltas between frames.
            for (int i = 0; i < event.getHistorySize(); i++) {
                sendCapturedMouseMotion(event.getHistoricalX(0, i),
                        event.getHistoricalY(0, i));
            }
            sendCapturedMouseMotion(event.getX(), event.getY());
        }

        if (action == MotionEvent.ACTION_SCROLL) {
            float vScroll = event.getAxisValue(MotionEvent.AXIS_VSCROLL);
            float hScroll = event.getAxisValue(MotionEvent.AXIS_HSCROLL);
            if (vScroll != 0f)
                mNative.sendMouseScroll(0, -vScroll * 10f);
            if (hScroll != 0f)
                mNative.sendMouseScroll(1, hScroll * 10f);
        }

        // Button state is present on motion, button, and down/up events.  Keeping
        // this diff in one place also handles mice that report button actions as
        // ACTION_BUTTON_PRESS/RELEASE instead of ACTION_DOWN/UP.
        if (action == MotionEvent.ACTION_CANCEL)
            releaseAllMouseButtons();
        else
            updateMouseButtonStateFromEvent(event);
        return true;
    }

    /**
     * Feed raw capture events through the original VirtualTouchpad recognizer,
     * while keeping raw relative-axis motion in the capture-specific backend.
     */
    private boolean handleCapturedTouchpadEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int pointerCount = event.getPointerCount();
        boolean hasButton = event.getButtonState() != 0
                || action == MotionEvent.ACTION_BUTTON_PRESS
                || action == MotionEvent.ACTION_BUTTON_RELEASE;
        boolean canceled = action == MotionEvent.ACTION_CANCEL
                || ((action == MotionEvent.ACTION_POINTER_UP
                    || action == MotionEvent.ACTION_UP)
                    && (event.getFlags() & MotionEvent.FLAG_CANCELED) != 0);
        int classification = event.getClassification();
        boolean unsupportedGesture = pointerCount >= 3
                || classification == CLASSIFICATION_PINCH
                || classification == CLASSIFICATION_MULTI_FINGER_SWIPE;
        boolean explicitScroll = (action == MotionEvent.ACTION_MOVE
                || action == MotionEvent.ACTION_HOVER_MOVE)
                && hasCapturedTouchpadScrollAxes(event);
        boolean scrollEvent = action == MotionEvent.ACTION_SCROLL || explicitScroll;

        if (canceled || unsupportedGesture) {
            if (capturedTouchpadRecognizer != null)
                capturedTouchpadRecognizer.cancel();
            capturedTouchpadBaselineValid = false;
        } else if (scrollEvent) {
            // Absolute capture normally supplies raw pointer coordinates, but a
            // few drivers still emit scroll axes. They must cancel a pending tap
            // before its final ACTION_UP.
            if (capturedTouchpadRecognizer != null)
                capturedTouchpadRecognizer.cancel();
            for (int i = 0; i < event.getHistorySize(); i++)
                sendCapturedTouchpadScrollAxes(event, i);
            sendCapturedTouchpadScrollAxes(event, -1);
            capturedTouchpadBaselineValid = false;
        } else if (hasButton) {
            if (capturedTouchpadRecognizer != null)
                capturedTouchpadRecognizer.cancel();
        } else if (capturedTouchpadRecognizer != null) {
            MotionEvent gestureEvent = normalizeCapturedTouchpadEvent(event);
            try {
                capturedTouchpadRecognizer.onTouch(gestureEvent);
            } finally {
                gestureEvent.recycle();
            }
        }

        // A physical button cancels tap recognition, but it must not stop the
        // relative cursor stream: touchpad click-drag still needs motion.
        if (!canceled && !unsupportedGesture && !scrollEvent) {
            switch (action) {
                case MotionEvent.ACTION_DOWN:
                    setCapturedTouchpadBaseline(event, -1);
                    break;
                case MotionEvent.ACTION_POINTER_DOWN:
                case MotionEvent.ACTION_POINTER_UP:
                    // A pointer-count transition must not create a cursor jump.
                    capturedTouchpadBaselineValid = false;
                    break;
                case MotionEvent.ACTION_MOVE:
                case MotionEvent.ACTION_HOVER_MOVE:
                    if (pointerCount == 1) {
                        for (int i = 0; i < event.getHistorySize(); i++)
                            processCapturedTouchpadMotionSample(event, i);
                        processCapturedTouchpadMotionSample(event, -1);
                    }
                    break;
                case MotionEvent.ACTION_UP:
                    capturedTouchpadBaselineValid = false;
                    break;
            }
        }

        if (action == MotionEvent.ACTION_CANCEL)
            releaseAllMouseButtons();
        else
            updateMouseButtonStateFromEvent(event);
        return true;
    }

    private void processCapturedTouchpadMotionSample(MotionEvent event,
                                                      int historyPos) {
        if (event.getPointerCount() != 1)
            return;
        float dx = capturedTouchpadAxis(event, MotionEvent.AXIS_RELATIVE_X,
                0, historyPos);
        float dy = capturedTouchpadAxis(event, MotionEvent.AXIS_RELATIVE_Y,
                0, historyPos);
        float[] fallback = applyCapturedTouchpadAbsoluteFallback(
                event, historyPos, dx, dy);
        sendCapturedTouchpadMotion(fallback[0], fallback[1]);
    }

    /**
     * Convert device-specific pad coordinates to view pixels for the original
     * recognizer. SOURCE_CLASS_POSITION ignores MotionEvent transforms on newer
     * Android releases, so use a temporary touchscreen source on the copy.
     */
    private MotionEvent normalizeCapturedTouchpadEvent(MotionEvent event) {
        MotionEvent copy = MotionEvent.obtain(event);
        copy.setSource(InputDevice.SOURCE_TOUCHSCREEN);

        int width = pointerViewWidth();
        int height = pointerViewHeight();
        InputDevice device = event.getDevice();
        InputDevice.MotionRange xRange = device == null ? null
                : device.getMotionRange(MotionEvent.AXIS_X,
                        InputDevice.SOURCE_TOUCHPAD);
        InputDevice.MotionRange yRange = device == null ? null
                : device.getMotionRange(MotionEvent.AXIS_Y,
                        InputDevice.SOURCE_TOUCHPAD);
        if (device != null && xRange == null)
            xRange = device.getMotionRange(MotionEvent.AXIS_X);
        if (device != null && yRange == null)
            yRange = device.getMotionRange(MotionEvent.AXIS_Y);

        if (width > 0 && height > 0 && xRange != null && yRange != null
                && xRange.getRange() > 0f && yRange.getRange() > 0f) {
            float sx = width / xRange.getRange();
            float sy = height / yRange.getRange();
            capturedTouchpadTransform.setScale(sx, sy);
            capturedTouchpadTransform.postTranslate(-xRange.getMin() * sx,
                    -yRange.getMin() * sy);
            copy.transform(capturedTouchpadTransform);
        }
        return copy;
    }

    /** Use absolute pad coordinates only when a driver omits the relative axes. */
    private float[] applyCapturedTouchpadAbsoluteFallback(MotionEvent event,
                                                           int historyPos,
                                                           float dx, float dy) {
        int pointerCount = event.getPointerCount();
        float centroidX = capturedTouchpadCentroid(event, historyPos, true);
        float centroidY = capturedTouchpadCentroid(event, historyPos, false);
        if (dx == 0f && dy == 0f
                && capturedTouchpadBaselineValid
                && capturedTouchpadBaselinePointers == pointerCount) {
            // AXIS_X/Y are device-dependent touchpad units. Normalize the
            // low-reliability fallback through the device motion ranges before
            // treating it as a display-pixel delta.
            dx = (centroidX - capturedTouchpadLastCentroidX)
                    * capturedTouchpadCoordinateScale(event, MotionEvent.AXIS_X, true);
            dy = (centroidY - capturedTouchpadLastCentroidY)
                    * capturedTouchpadCoordinateScale(event, MotionEvent.AXIS_Y, false);
        }
        capturedTouchpadLastCentroidX = centroidX;
        capturedTouchpadLastCentroidY = centroidY;
        capturedTouchpadBaselinePointers = pointerCount;
        capturedTouchpadBaselineValid = true;
        capturedTouchpadResolvedDelta[0] = dx;
        capturedTouchpadResolvedDelta[1] = dy;
        return capturedTouchpadResolvedDelta;
    }

    private float capturedTouchpadCoordinateScale(MotionEvent event, int axis,
                                                   boolean xAxis) {
        InputDevice device = event.getDevice();
        if (device == null)
            return 0f;
        InputDevice.MotionRange range = device.getMotionRange(
                axis, InputDevice.SOURCE_TOUCHPAD);
        if (range == null)
            range = device.getMotionRange(axis);
        float span = range == null ? 0f : range.getRange();
        int size = xAxis ? pointerViewWidth() : pointerViewHeight();
        return span > 0f && size > 0 ? size / span : 0f;
    }

    private void sendCapturedTouchpadMotion(float dx, float dy) {
        if (dx == 0f && dy == 0f)
            return;
        float distance = (float) Math.hypot(dx, dy);
        float speed = distance / 10.0f;
        float scale = 1.0f + (capturedTouchpadAccel - 1.0f)
                * (speed / (1.0f + speed));
        scale = Math.max(0.3f, Math.min(10.0f, scale));
        sendCapturedMouseMotion(dx * scale, dy * scale);
    }

    private void sendCapturedTouchpadScrollAxes(MotionEvent event, int historyPos) {
        if (event.getPointerCount() <= 0)
            return;
        float vScroll = capturedTouchpadAxis(event, MotionEvent.AXIS_VSCROLL,
                0, historyPos);
        float hScroll = capturedTouchpadAxis(event, MotionEvent.AXIS_HSCROLL,
                0, historyPos);
        if (vScroll != 0f || hScroll != 0f) {
            if (vScroll != 0f)
                mNative.sendMouseScroll(0, -vScroll * 10f);
            if (hScroll != 0f)
                mNative.sendMouseScroll(1, hScroll * 10f);
            return;
        }

        float gestureX = capturedTouchpadAxis(event,
                MotionEvent.AXIS_GESTURE_SCROLL_X_DISTANCE, 0, historyPos);
        float gestureY = capturedTouchpadAxis(event,
                MotionEvent.AXIS_GESTURE_SCROLL_Y_DISTANCE, 0, historyPos);
        if (gestureY != 0f)
            mNative.sendMouseScroll(0, gestureY);
        if (gestureX != 0f)
            mNative.sendMouseScroll(1, -gestureX);
    }

    private boolean hasCapturedTouchpadScrollAxes(MotionEvent event) {
        if (event.getPointerCount() <= 0)
            return false;
        for (int i = 0; i < event.getHistorySize(); i++) {
            if (capturedTouchpadAxis(event, MotionEvent.AXIS_VSCROLL, 0, i) != 0f
                    || capturedTouchpadAxis(event, MotionEvent.AXIS_HSCROLL, 0, i) != 0f
                    || capturedTouchpadAxis(event,
                            MotionEvent.AXIS_GESTURE_SCROLL_X_DISTANCE, 0, i) != 0f
                    || capturedTouchpadAxis(event,
                            MotionEvent.AXIS_GESTURE_SCROLL_Y_DISTANCE, 0, i) != 0f)
                return true;
        }
        return capturedTouchpadAxis(event, MotionEvent.AXIS_VSCROLL, 0, -1) != 0f
                || capturedTouchpadAxis(event, MotionEvent.AXIS_HSCROLL, 0, -1) != 0f
                || capturedTouchpadAxis(event,
                        MotionEvent.AXIS_GESTURE_SCROLL_X_DISTANCE, 0, -1) != 0f
                || capturedTouchpadAxis(event,
                        MotionEvent.AXIS_GESTURE_SCROLL_Y_DISTANCE, 0, -1) != 0f;
    }

    private float capturedTouchpadAxis(MotionEvent event, int axis,
                                       int pointerIndex, int historyPos) {
        return historyPos >= 0
                ? event.getHistoricalAxisValue(axis, pointerIndex, historyPos)
                : event.getAxisValue(axis, pointerIndex);
    }

    private float capturedTouchpadCentroid(MotionEvent event, int historyPos,
                                           boolean xAxis) {
        int pointerCount = event.getPointerCount();
        if (pointerCount <= 0)
            return 0f;
        float total = 0f;
        for (int i = 0; i < pointerCount; i++) {
            if (historyPos >= 0) {
                total += xAxis ? event.getHistoricalX(i, historyPos)
                        : event.getHistoricalY(i, historyPos);
            } else {
                total += xAxis ? event.getX(i) : event.getY(i);
            }
        }
        return total / pointerCount;
    }

    private void setCapturedTouchpadBaseline(MotionEvent event, int historyPos) {
        capturedTouchpadLastCentroidX = capturedTouchpadCentroid(
                event, historyPos, true);
        capturedTouchpadLastCentroidY = capturedTouchpadCentroid(
                event, historyPos, false);
        capturedTouchpadBaselinePointers = event.getPointerCount();
        capturedTouchpadBaselineValid = capturedTouchpadBaselinePointers > 0;
    }

    private void resetCapturedTouchpadGesture() {
        if (capturedTouchpadRecognizer != null)
            capturedTouchpadRecognizer.cancel();
        capturedTouchpadBaselineValid = false;
        capturedTouchpadBaselinePointers = 0;
        capturedTouchpadLastCentroidX = 0f;
        capturedTouchpadLastCentroidY = 0f;
    }

    private void sendCapturedMouseMotion(float dx, float dy) {
        if (!Float.isFinite(dx) || !Float.isFinite(dy)
                || (dx == 0f && dy == 0f))
            return;
        ensurePointerPosition();
        int width = pointerViewWidth();
        int height = pointerViewHeight();
        if (width <= 0 || height <= 0)
            return;

        float originX = pointerOriginX();
        float originY = pointerOriginY();
        pointerX = Math.max(originX, Math.min(pointerX + dx, originX + width));
        pointerY = Math.max(originY, Math.min(pointerY + dy, originY + height));
        float scaleX = pointerScaleX();
        float scaleY = pointerScaleY();
        // Keep the absolute position clamped, but preserve the raw relative delta
        // (scaled into the output coordinate space).  This is important for games:
        // movement continues to be reported even while the virtual cursor is at an
        // output edge.
        mNative.sendMouseMotion((pointerX - originX) * scaleX,
                (pointerY - originY) * scaleY,
                dx * scaleX, dy * scaleY);
    }

    @Override
    protected void onResume() {
        super.onResume();

        // Bounced to Settings from onCreate (socket missing): nothing was set up, so
        // just exit this window instead of running the connect logic.
        if (mForceSettings) {
            finish();
            return;
        }

        // Show settings notification while in foreground, unless disabled in Settings.
        if (getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .getBoolean(KEY_NOTIFICATION_ENABLED, true)) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                    && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                            != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 1003);
            } else {
                showSettingsNotification();
            }
        } else {
            NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            if (nm != null) nm.cancel(NOTIFICATION_ID);
        }

        // Re-check accessibility service state on resume
        KeyInterceptor.recheck();

        // If the user edited the layout JSON in Settings, rebuild the bar so the
        // change takes effect on return to the desktop.
        String layoutJson = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .getString(KEY_EXTRA_KEYS_LAYOUT, "");
        if (!layoutJson.equals(mAppliedLayoutJson))
            rebuildExtraKeysBar();

        // Pick up a Keyboard-floating toggle made in Settings: update the bar's
        // backdrop and re-run the layout so the surface margin tracks the new mode.
        mKeyboardFloating = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .getBoolean(KEY_KEYBOARD_FLOATING, true);
        if (extraKeysBar != null)
            extraKeysBar.setFloating(mKeyboardFloating);
        relayout();

        // Sync extra-keys bar visibility with the settings switches. With auto-show
        // ON the bar tracks the keyboard (hidden now if the IME isn't up); with it
        // OFF the master switch decides. See shouldShowBar.
        setExtraKeysBarVisible(shouldShowBar(systemIme.isImeVisible()));

        setupFullscreen();
        DisplayManager dm = getSystemService(DisplayManager.class);
        if (dm != null)
            dm.registerDisplayListener(displayListener, null);
        // Bring the camera service up (or confirm it disabled) BEFORE nativeStart, so
        // the render thread's do_connect() sees a settled camera_service_is_ready()
        // and registers SERVICE_TYPE_CAMERA on the very first connect rather than a
        // later reconnect. Idempotent, so safe to call on every resume.
        applyCameraState();
        if (surfaceReady) {
            mNative.stop();
            applyConnectionConfig();
            startNative(surfaceView.getHolder().getSurface());
            pushRefreshRate();
            applyMicState();
            applyAudioLatency();
        }

        // ===== 重新读取触摸板设置 =====
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        isTouchpadMode = prefs.getBoolean(KEY_TOUCHPAD_MODE, false);
        capturedTouchpadAccel = Math.max(0.5f,
                Math.min(10.0f, prefs.getFloat(KEY_MOUSE_ACCEL, 1.0f)));
        virtualTouchpad.setAccelStrength(capturedTouchpadAccel);
        pointerCaptureEnabled = prefs.getBoolean(KEY_POINTER_CAPTURE, false);
        // A manual Back-key release lasts until the next click or lifecycle
        // transition. Returning from Settings starts a fresh capture session.
        pointerCaptureSuppressed = false;
        if (mRoot != null)
            mRoot.post(this::syncPointerCapture);
        autoStretch = prefs.getBoolean(KEY_AUTO_STRETCH, true);
        relayout();

        // Pick up an immersive-capture toggle made in Settings. Resuming never immerses
        // by itself -- it only arms the toggle key and makes sure nothing is still grabbed.
        fullKeyCaptureEnabled = prefs.getBoolean(KEY_FULL_KEY_CAPTURE, false);
        immersionKeycode = prefs.getInt(KEY_IMMERSION_KEYCODE, UNBOUND);
        immersionScancode = prefs.getInt(KEY_IMMERSION_SCANCODE, 0);
        immersed = false;
        stopImmersiveGrab();

        // The socket pref may have been edited in Settings; keep our dedup key current.
        registerWindow();
    }

    @Override
    protected void onPause() {
        super.onPause();
        // Socket-missing bounce: no pipeline exists, so skip teardown (mNative is
        // null) and don't let the jump to Settings trigger any of it.
        if (mForceSettings) return;
        clearPointerCaptureBackTracking();
        releasePointerCapture(false);
        immersed = false;
        stopImmersiveGrab();
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm != null) nm.cancel(NOTIFICATION_ID);
        DisplayManager dm = getSystemService(DisplayManager.class);
        if (dm != null)
            dm.unregisterDisplayListener(displayListener);
        mNative.stop();
    }

    @Override
    protected void onDestroy() {
        releasePointerCapture(false);
        if (mRegisteredSocket != null) {
            sWindowsBySocket.remove(mRegisteredSocket, this);
            mRegisteredSocket = null;
        }
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm != null) nm.cancel(NOTIFICATION_ID);
        // Release only THIS window's native pipeline (its consumer_state, audio bridge
        // and camera client). The camera service itself is a process-global shared by
        // every window, so it is intentionally not torn down here -- destroying it
        // would cut the camera for the other open windows.
        if (mNative != null) {
            // Destroy on the grab worker, not here: nativeDestroy joins the grab
            // thread, which can be parked in waitpid() on `su`. Queued behind any
            // pending start/stop on this single-thread executor, so free() can never
            // race them. Blocking the main thread here was an ANR plus a use-after-free.
            final Native n = mNative;
            mNative = null;
            postGrabTask(n::destroy);
            grabExecutor.shutdown();
        }
        cameraInited = false;
        super.onDestroy();
    }

    /*
     * Bring the camera service up only when the user enabled it AND CAMERA is
     * granted. The native fds/threads are created once and persist across transport
     * restarts, so this is idempotent (guarded by cameraInited). When the toggle is
     * off we never init, so do_connect() never registers SERVICE_TYPE_CAMERA and the
     * producer never sees it. Request the permission if enabled but not yet granted;
     * onRequestPermissionsResult finishes the init.
     */
    private void applyCameraState() {
        boolean want = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .getBoolean(KEY_CAMERA_ENABLED, false);
        if (!want || cameraInited)
            return;
        if (checkSelfPermission(Manifest.permission.CAMERA)
                == PackageManager.PERMISSION_GRANTED) {
            CameraServices.nativeInitCameraService(this);
            cameraInited = true;
        } else {
            requestPermissions(new String[]{Manifest.permission.CAMERA}, REQ_CAMERA);
        }
    }

    /*
     * Forward the mic only when the user enabled it AND RECORD_AUDIO is granted.
     * If enabled but not yet granted, request it; onRequestPermissionsResult applies
     * the result. Safe to call after every nativeStart (re)connect.
     */
    /* Push the speaker/mic latency presets to native (which forwards them to the
     * producer's PipeWire nodes). Safe to call after every (re)connect and whenever
     * the user changes a preset. */
    private void applyAudioLatency() {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        int speakerMs = prefs.getInt(KEY_SPEAKER_LATENCY_MS, 0);
        int micMs = prefs.getInt(KEY_MIC_LATENCY_MS, 0);
        mNative.setAudioLatency(speakerMs, micMs);
    }

    private void applyMicState() {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        boolean want = prefs.getBoolean(KEY_MIC_ENABLED, false);
        if (!want) {
            mNative.setMicEnabled(false);
            return;
        }
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED) {
            mNative.setMicEnabled(true);
        } else {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO},
                               REQ_RECORD_AUDIO);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_RECORD_AUDIO) {
            boolean granted = grantResults.length > 0
                    && grantResults[0] == PackageManager.PERMISSION_GRANTED;
            mNative.setMicEnabled(granted);
        } else if (requestCode == REQ_CAMERA) {
            boolean granted = grantResults.length > 0
                    && grantResults[0] == PackageManager.PERMISSION_GRANTED;
            if (granted && !cameraInited && getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                    .getBoolean(KEY_CAMERA_ENABLED, false)) {
                CameraServices.nativeInitCameraService(this);
                cameraInited = true;
            }
        } else if (requestCode == 1003) {
            if (getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                    .getBoolean(KEY_NOTIFICATION_ENABLED, true)) {
                showSettingsNotification();
            }
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Log.i(TAG, "surfaceChanged: " + width + "x" + height);
        viewWidth = width;
        viewHeight = height;
        ensurePointerPosition();
        surfaceReady = true;
        // Same ordering guarantee as onResume: camera service settled before connect.
        applyCameraState();
        mNative.stop();
        applyConnectionConfig();
        startNative(holder.getSurface());
        pushRefreshRate();
        applyMicState();
        applyAudioLatency();

        // ===== 更新屏幕尺寸并重置平滑状态 =====
        virtualTouchpad.onSurfaceChanged();
        if (mRoot != null)
            mRoot.post(this::syncPointerCapture);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        releasePointerCapture(false);
        mNative.stop();
    }


    // Shrink the surface to the area above the keyboard (and the extra-keys bar,
    // if shown) by giving it a bottom margin. The size change flows through
    // surfaceChanged -> nativeStart and the producer's resize path, so the
    // focused window relayouts into the upper region instead of hiding behind
    // the keyboard. Reset when the IME goes away.
    private void applyImeInset(WindowInsets insets) {
        int newImeBottom = insets.getInsets(WindowInsets.Type.ime()).bottom;
        boolean imeVisible = newImeBottom > 0;
        boolean wasImeVisible = mImeBottom > 0;
    
        mImeBottom = newImeBottom;
    
        // Only "with_keyboard" mode tracks the IME; "always"/"never"
        // let the user's manual toggle (back key) stay untouched.
        String mode = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .getString(KEY_EXTRA_KEYS_MODE, "always");
        if (imeVisible != wasImeVisible && "with_keyboard".equals(mode))
            setExtraKeysBarVisible(imeVisible);

        relayout();
    }

    // Desired extra-keys bar visibility for the current keyboard state. The
    // single three-way preference replaces the old two-switch pair:
    //   "always"       – bar always visible
    //   "never"        – bar always hidden
    //   "with_keyboard" – bar tracks the soft keyboard (default)
    private boolean shouldShowBar(boolean imeVisible) {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        String mode = prefs.getString(KEY_EXTRA_KEYS_MODE, "always");
        switch (mode) {
            case "always":   return true;
            case "never":    return false;
            default:         return imeVisible;
        }
    }

    // Recompute the surface bottom margin and the bar position from the current
    // IME inset and bar visibility. The surface ends above the bar, which sits
    // directly on top of the IME: "surface / extra-keys bar / IME" bottom-up.
    private void relayout() {
        boolean barVisible = extraKeysBar != null && extraKeysBar.getVisibility() == View.VISIBLE;
        int barH = barVisible ? mBarHeight : 0;
        // Floating mode: keyboard + bar overlay the display, so the surface keeps
        // its full size (target 0). Default mode: shrink the surface above both.
        int target = mKeyboardFloating ? 0 : (mImeBottom + barH);

        FrameLayout.LayoutParams lp =
            (FrameLayout.LayoutParams) surfaceView.getLayoutParams();
        
        if (autoStretch) {
            lp.width = FrameLayout.LayoutParams.MATCH_PARENT;
            lp.height = FrameLayout.LayoutParams.MATCH_PARENT;
            lp.gravity = Gravity.NO_GRAVITY;
            lp.leftMargin = 0;
            lp.topMargin = 0;
            if (lp.bottomMargin != target) {
                lp.bottomMargin = target;
                surfaceView.setLayoutParams(lp);
            }
        } else {
            lp.bottomMargin = target;
            adjustSurfaceViewLayout(lp);
        }
        
        if (extraKeysBar != null)
            extraKeysBar.setTranslationY(-mImeBottom);
    }
    
    private void adjustSurfaceViewLayout(FrameLayout.LayoutParams lp) {
        if (customScreenWidth <= 0 || customScreenHeight <= 0 || mRoot == null) {
            lp.width = FrameLayout.LayoutParams.MATCH_PARENT;
            lp.height = FrameLayout.LayoutParams.MATCH_PARENT;
            lp.gravity = Gravity.NO_GRAVITY;
            lp.leftMargin = 0;
            lp.topMargin = 0;
            surfaceView.setLayoutParams(lp);
            surfaceOffsetX = 0f;
            surfaceOffsetY = 0f;
            surfaceScale = 1f;
            return;
        }
        
        int parentW = mRoot.getWidth();
        int parentH = mRoot.getHeight();
        if (parentW <= 0 || parentH <= 0) {
            return;
        }
        
        float customRatio = (float) customScreenWidth / customScreenHeight;
        float screenRatio = (float) parentW / parentH;
        
        int surfaceW, surfaceH;
        if (customRatio > screenRatio) {
            surfaceW = parentW;
            surfaceH = (int) (parentW / customRatio);
        } else {
            surfaceH = parentH;
            surfaceW = (int) (parentH * customRatio);
        }
        
        lp.width = surfaceW;
        lp.height = surfaceH;
        lp.gravity = Gravity.CENTER;
        lp.leftMargin = 0;
        lp.topMargin = 0;
        surfaceView.setLayoutParams(lp);
        
        surfaceOffsetX = (parentW - surfaceW) / 2f;
        surfaceOffsetY = (parentH - surfaceH) / 2f;
        surfaceScale = (float) surfaceW / customScreenWidth;
    }

    // Show/hide the extra-keys bar and re-apply the layout so the display area
    // is compressed (shown) or restored (hidden).
    private void setExtraKeysBarVisible(boolean visible) {
        if (extraKeysBar == null) return;
        boolean cur = extraKeysBar.getVisibility() == View.VISIBLE;
        if (cur == visible) return;
        extraKeysBar.setVisibility(visible ? View.VISIBLE : View.GONE);
        if (!visible) extraKeysBar.reset();
        relayout();
    }

    // Construct the extra-keys bar from the user's saved JSON layout and add it to
    // the content root (hidden). The bar height mirrors Termux at 37.5dp/row and
    // scales with the parsed row count. Records the layout JSON it was built from.
    private void buildExtraKeysBar() {
        extraKeysBar = new ExtraKeysBar(this, new ExtraKeysBar.Sender() {
            @Override public void key(int action, int evdev) { mNative.sendKey(action, evdev); }
            @Override public void text(String s) {
                if (!s.isEmpty()) mNative.sendTextInput(s.getBytes(StandardCharsets.UTF_8));
            }
            // Tapping the ⌨ key keeps the original behaviour: toggle the system IME.
            @Override public void toggleKeyboard() { systemIme.toggleSystemKeyboard(); }
            // Pulling up on the ⌨ key toggles the floating virtual keyboard.
            @Override public void toggleVirtualKeyboard() {
                if (virtualKeyboardView.getVisibility() == View.VISIBLE) {
                    virtualKeyboardView.setVisibility(View.GONE);
                } else {
                    Log.d("VirtualKeyboard", "toggle: showing keyboard, mRoot="
                            + mRoot.getWidth() + "x" + mRoot.getHeight());
                    virtualKeyboardView.setVisibility(View.VISIBLE);
                    virtualKeyboardView.bringToFront();
                    // Re-position it (in case screen size changed)
                    positionVirtualKeyboard();
                    // Hide the system IME to avoid overlap with the floating keyboard.
                    InputMethodManager imm = getSystemService(InputMethodManager.class);
                    if (imm != null && getCurrentFocus() != null) {
                        imm.hideSoftInputFromWindow(getCurrentFocus().getWindowToken(), 0);
                    }
                }
            }
            @Override public void openSettings() {
                startActivity(new Intent(MainActivity.this, SettingsActivity.class));
            }
        });
        mBarHeight = Math.round(37.5f * mDensity * extraKeysBar.getRowCount());
        extraKeysBar.setFloating(mKeyboardFloating);
        extraKeysBar.setVisibility(View.GONE);
        mRoot.addView(extraKeysBar, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, mBarHeight, Gravity.BOTTOM));
        mAppliedLayoutJson = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .getString(KEY_EXTRA_KEYS_LAYOUT, "");
    }

    // Replace the bar with a freshly-parsed one after the user edits the layout in
    // Settings. Called from onResume when the saved JSON no longer matches what the
    // current bar was built from.
    private void rebuildExtraKeysBar() {
        if (mRoot == null) return;
        if (extraKeysBar != null) {
            extraKeysBar.reset();
            mRoot.removeView(extraKeysBar);
        }
        buildExtraKeysBar();
        setExtraKeysBarVisible(shouldShowBar(systemIme.isImeVisible()));
        relayout();
    }

    // Toggle the extra-keys bar on its own (e.g. from the Back key), independent of
    // the soft keyboard. Showing it just compresses the display area above the bar.
    private void toggleExtraKeysBar() {
        boolean visible = extraKeysBar != null
            && extraKeysBar.getVisibility() == View.VISIBLE;
        setExtraKeysBarVisible(!visible);
    }

    // Apply the screen-orientation preference (default / landscape / portrait).
    private void applyOrientation() {
        String mode = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .getString("screen_orientation", "default");
        switch (mode) {
            case "landscape":
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
                break;
            case "portrait":
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT);
                break;
            default:
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
                break;
        }
    }

    // ---- SystemIME.Host ----

    @Override
    public ExtraKeysBar getExtraKeysBar() {
        return extraKeysBar;
    }

    // The IME was shown/hidden via SystemIME's toggle. In freeform mode the inset
    // callback may not fire, so sync the extra-keys bar explicitly here in all modes.
    @Override
    public void onImeVisibilityChanged(boolean visible) {
        String mode = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .getString(KEY_EXTRA_KEYS_MODE, "always");
        if ("with_keyboard".equals(mode))
            setExtraKeysBarVisible(visible);
        if (!visible && surfaceView != null) {
            surfaceView.requestFocus();
            if (mRoot != null)
                mRoot.post(this::syncPointerCapture);
        }
    }

    // The bound key toggles immersion. Intercepted here rather than in onKeyDown so it
    // is seen before the key would be forwarded to Linux.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (handleImmersionKey(event))
            return true;
        return super.dispatchKeyEvent(event);
    }

    // ================================================================
    // Route touchscreen gestures and ordinary (non-captured) mouse events.
    // ================================================================
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        boolean mouseEvent = isMouseEvent(event);
        if (mouseEvent && event.getActionMasked() == MotionEvent.ACTION_DOWN
                && pointerCaptureEnabled && mRoot != null
                && !mRoot.hasPointerCapture()) {
            // A Back-key release leaves the pointer free long enough for the user
            // to move/click normally; the next click starts a new capture session.
            requestPointerCaptureAfterClick();
        }

        // ===== 触摸板模式优先处理（仅针对非鼠标触摸事件） =====
        if (isTouchpadMode && !mouseEvent) {
            return virtualTouchpad.onTouch(event);
        }

        if (mouseEvent) {
            int cls = event.getClassification();
            if (cls == CLASSIFICATION_TWO_FINGER_SWIPE)
                return handleTouchpadScroll(event);
            if (cls == CLASSIFICATION_MULTI_FINGER_SWIPE || cls == CLASSIFICATION_PINCH)
                return handleTouchEvent(event);
            return handleMouseEvent(event);
        }
        return handleTouchEvent(event);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        if (isMouseEvent(event)) {
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_BUTTON_PRESS
                    && pointerCaptureEnabled && mRoot != null
                    && !mRoot.hasPointerCapture()) {
                requestPointerCaptureAfterClick();
            }
            if (action == MotionEvent.ACTION_HOVER_MOVE) {
                float nativeX, nativeY;
                if (autoStretch) {
                    float scaleX = (customScreenWidth > 0 && viewWidth > 0) ? 
                            (float)customScreenWidth / viewWidth : 1.0f;
                    float scaleY = (customScreenHeight > 0 && viewHeight > 0) ? 
                            (float)customScreenHeight / viewHeight : 1.0f;
                    nativeX = event.getX() * scaleX;
                    nativeY = event.getY() * scaleY;
                } else {
                    nativeX = (event.getX() - surfaceOffsetX) / surfaceScale;
                    nativeY = (event.getY() - surfaceOffsetY) / surfaceScale;
                }
        
                pointerX = event.getX();
                pointerY = event.getY();
                ensurePointerPosition();
                mNative.sendMouseMotion(nativeX, nativeY,
                                      event.getAxisValue(MotionEvent.AXIS_RELATIVE_X),
                                      event.getAxisValue(MotionEvent.AXIS_RELATIVE_Y));
                return true;
            }
            if (action == MotionEvent.ACTION_SCROLL) {
                float vScroll = event.getAxisValue(MotionEvent.AXIS_VSCROLL);
                float hScroll = event.getAxisValue(MotionEvent.AXIS_HSCROLL);
                if (vScroll != 0)
                    mNative.sendMouseScroll(0, -vScroll * 10);
                if (hScroll != 0)
                    mNative.sendMouseScroll(1, hScroll * 10);
                return true;
            }
            if (action == MotionEvent.ACTION_BUTTON_PRESS
                    || action == MotionEvent.ACTION_BUTTON_RELEASE) {
                updateMouseButtonStateFromEvent(event);
                return true;
            }
        }
        return super.onGenericMotionEvent(event);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (handlePointerCaptureBackKey(event))
            return true;
        // Mouse Back is already forwarded as BTN_SIDE from the MotionEvent path.
        // Swallow Android's duplicate KEYCODE_BACK so it neither releases capture
        // nor toggles the extra-keys bar.
        if (keyCode == KeyEvent.KEYCODE_BACK && isMouseKeyEvent(event))
            return true;
        if (event.getRepeatCount() > 0)
            return true;

        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        int boundKeycode = prefs.getInt(KEY_BOUND_KEYCODE, -1);
        if (boundKeycode != -1 && keyCode == boundKeycode) {
            systemIme.toggleSystemKeyboard();   // Keep original bound key behavior (system IME)
            return true;
        }

        // Back key toggles the extra-keys bar (without opening the soft keyboard)
        // when enabled in settings. Leaves the default swallow behaviour otherwise.
        if (keyCode == KeyEvent.KEYCODE_BACK
                && prefs.getBoolean(KEY_BACK_OPENS_EXTRA_KEYS, true)) {
            toggleExtraKeysBar();
            return true;
        }

        forwardKeyToLinux(event);
        return true;
    }

    // Some OEM ROMs (notably Xiaomi/HyperOS) dispatch Back via onBackPressed()
    // instead of onKeyDown(). Release an active pointer capture here; otherwise
    // keep swallowing Back (same approach as Termux-X11) so the Activity does not
    // unexpectedly finish via gesture navigation.
    @Override
    public void onBackPressed() {
        if (mRoot != null && mRoot.hasPointerCapture()) {
            releasePointerCapture(true);
            // There is no KeyEvent on this OEM path. Use a wildcard so a trailing
            // Back UP, if one still arrives, is consumed; a fresh DOWN clears it.
            trackPointerCaptureBack(null);
            showPointerCaptureReleasedToast();
            return;
        }
    }

    // Called from KeyInterceptor (accessibility service) to handle keys that
    // the normal onKeyDown/onKeyUp might miss (e.g. Fn combos).
    public boolean handleAccessibilityKey(KeyEvent event) {
        // Check the toggle here too: when accessibility interception is on, this path
        // consumes every key BEFORE the window sees it, so dispatchKeyEvent would never
        // run and immersion could never be entered. Whichever path delivers the key,
        // the immersion toggle gets first look.
        if (handleImmersionKey(event))
            return true;
        if (handlePointerCaptureBackKey(event))
            return true;
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && isMouseKeyEvent(event))
            return true;
        if (event.getRepeatCount() > 0)
            return true;

        // Some tablet keyboard layouts expose their physical Esc key as
        // Android Back (Linux KEY_BACK / Browser Back).  Convert it only on
        // the accessibility-interception path so the normal Android Back and
        // extra-keys-bar behaviour is unchanged when interception is off.
        return forwardKeyToLinux(event, true);
    }

    private boolean forwardKeyToLinux(KeyEvent event) {
        return forwardKeyToLinux(event, false);
    }

    private boolean forwardKeyToLinux(KeyEvent event, boolean convertBackToEscape) {
        int keyCode = event.getKeyCode();
        int action = event.getAction() == KeyEvent.ACTION_DOWN ? 0 : 1;
        int evdev = -1;

        if (convertBackToEscape
                && (keyCode == KeyEvent.KEYCODE_BACK
                    || event.getScanCode() == EVDEV_BROWSER_BACK))
            evdev = KeyCodeMapper.getScanCode(KeyEvent.KEYCODE_ESCAPE);

        // Reserved Android keys may carry vendor scan codes that Linux does not
        // recognize, so prefer their explicit evdev mapping.
        if (evdev == -1 && shouldPreferMappedKey(keyCode))
            evdev = KeyCodeMapper.getScanCode(keyCode);

        if (evdev == -1 && event.getScanCode() != 0)
            evdev = event.getScanCode();

        if (evdev == -1)
            evdev = KeyCodeMapper.getScanCode(keyCode);

        if (evdev == -1)
            return false;

        mNative.sendKey(action, evdev);
        return true;
    }

    private static boolean shouldPreferMappedKey(int keyCode) {
        return keyCode == KeyEvent.KEYCODE_META_LEFT
                || keyCode == KeyEvent.KEYCODE_META_RIGHT
                || keyCode == KeyEvent.KEYCODE_SEARCH
                || keyCode == KeyEvent.KEYCODE_ASSIST
                || (keyCode >= KeyEvent.KEYCODE_F13 && keyCode <= KeyEvent.KEYCODE_F24);
    }

    public boolean isAccessibilityInterceptEnabled() {
        return getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .getBoolean(KEY_ACCESSIBILITY_ENABLED, false);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (handlePointerCaptureBackKey(event))
            return true;
        if (keyCode == KeyEvent.KEYCODE_BACK && isMouseKeyEvent(event))
            return true;
        forwardKeyToLinux(event);
        return true;
    }

    private static final int CLASSIFICATION_TWO_FINGER_SWIPE = 3;
    private static final int CLASSIFICATION_MULTI_FINGER_SWIPE = 4;
    private static final int CLASSIFICATION_PINCH = 5;

    private int savedBS = 0;

    private static final int[][] BUTTON_MAP = {
        {MotionEvent.BUTTON_PRIMARY,   0x110}, // BTN_LEFT
        {MotionEvent.BUTTON_SECONDARY, 0x111}, // BTN_RIGHT
        {MotionEvent.BUTTON_TERTIARY,  0x112}, // BTN_MIDDLE
        {MotionEvent.BUTTON_BACK,      0x113}, // BTN_SIDE
        {MotionEvent.BUTTON_FORWARD,   0x114}, // BTN_EXTRA
    };

    private void updateMouseButtonState(int currentBS) {
        for (int[] btn : BUTTON_MAP) {
            boolean wasDown = (savedBS & btn[0]) != 0;
            boolean isDown  = (currentBS & btn[0]) != 0;
            if (wasDown != isDown && mNative != null)
                mNative.sendMouseButton(btn[1], isDown);
        }
        savedBS = currentBS;
    }

    // ACTION_BUTTON_RELEASE may report a stale buttonState on some touchpad
    // drivers. Use the explicit action button as a correction when available.
    private void updateMouseButtonStateFromEvent(MotionEvent event) {
        int buttonState = event.getButtonState();
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_BUTTON_PRESS)
            buttonState |= event.getActionButton();
        else if (action == MotionEvent.ACTION_BUTTON_RELEASE)
            buttonState &= ~event.getActionButton();
        updateMouseButtonState(buttonState);
    }

    private void releaseAllMouseButtons() {
        if (savedBS == 0)
            return;
        for (int[] btn : BUTTON_MAP) {
            if ((savedBS & btn[0]) != 0 && mNative != null)
                mNative.sendMouseButton(btn[1], false);
        }
        savedBS = 0;
    }

    private boolean isMouseEvent(MotionEvent event) {
        int source = event.getSource();
        if ((source & InputDevice.SOURCE_TOUCHSCREEN) == InputDevice.SOURCE_TOUCHSCREEN)
            return false;
        if ((source & InputDevice.SOURCE_MOUSE) != InputDevice.SOURCE_MOUSE)
            return false;
        int toolType = event.getToolType(event.getActionIndex());
        return toolType == MotionEvent.TOOL_TYPE_MOUSE
            || toolType == MotionEvent.TOOL_TYPE_FINGER;
    }

    private boolean isMouseKeyEvent(KeyEvent event) {
        return event.isFromSource(InputDevice.SOURCE_MOUSE)
                || event.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE);
    }

    private boolean handleMouseEvent(MotionEvent event) {
        float dx = 0f;
        float dy = 0f;
        
        float nativeX, nativeY;
        if (autoStretch) {
            float scaleX = (customScreenWidth > 0 && viewWidth > 0) ? 
                       (float)customScreenWidth / viewWidth : 1.0f;
            float scaleY = (customScreenHeight > 0 && viewHeight > 0) ? 
                       (float)customScreenHeight / viewHeight : 1.0f;
            nativeX = event.getX() * scaleX;
            nativeY = event.getY() * scaleY;
            if (event.getHistorySize() > 0) {
                int last = event.getHistorySize() - 1;
                dx = (event.getX() - event.getHistoricalX(0, last))*scaleX;
                dy = (event.getY() - event.getHistoricalY(0, last))*scaleY;
            }
        } else {
            nativeX = (event.getX() - surfaceOffsetX) / surfaceScale;
            nativeY = (event.getY() - surfaceOffsetY) / surfaceScale;
            if (event.getHistorySize() > 0) {
                int last = event.getHistorySize() - 1;
                dx = (event.getX() - event.getHistoricalX(0, last)) / surfaceScale;
                dy = (event.getY() - event.getHistoricalY(0, last)) / surfaceScale;
            }
        }
        if (Float.isFinite(event.getX()) && Float.isFinite(event.getY())) {
            pointerX = event.getX();
            pointerY = event.getY();
            ensurePointerPosition();
        }
        mNative.sendMouseMotion(nativeX, nativeY, dx, dy);

        updateMouseButtonStateFromEvent(event);
        return true;
    }

    private boolean handleTouchpadScroll(MotionEvent event) {
        if (event.getActionMasked() == MotionEvent.ACTION_MOVE) {
            float scrollX = event.getAxisValue(MotionEvent.AXIS_GESTURE_SCROLL_X_DISTANCE);
            float scrollY = event.getAxisValue(MotionEvent.AXIS_GESTURE_SCROLL_Y_DISTANCE);
            if (scrollY != 0)
                mNative.sendMouseScroll(0, scrollY);
            if (scrollX != 0)
                mNative.sendMouseScroll(1, -scrollX);
        }
        return true;
    }

    // 原有 handleTouchEvent 一字未改
    private boolean handleTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int pointerIdx = event.getActionIndex();
        int pointerId = event.getPointerId(pointerIdx);
    
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                float[] downCoords = convertToNativeCoords(event.getX(pointerIdx), event.getY(pointerIdx));
                mNative.sendTouch(0, downCoords[0], downCoords[1], pointerId);
                mNative.sendTouchFrame();
                return true;
            
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                float[] upCoords = convertToNativeCoords(event.getX(pointerIdx), event.getY(pointerIdx));
                mNative.sendTouch(1, upCoords[0], upCoords[1], pointerId);
                mNative.sendTouchFrame();
                return true;
            
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); i++) {
                    float[] moveCoords = convertToNativeCoords(event.getX(i), event.getY(i));
                    mNative.sendTouch(2, moveCoords[0], moveCoords[1], event.getPointerId(i));
                }
                mNative.sendTouchFrame();
                return true;
            
            case MotionEvent.ACTION_CANCEL:
                for (int i = 0; i < event.getPointerCount(); i++) {
                    float[] cancelCoords = convertToNativeCoords(event.getX(i), event.getY(i));
                    mNative.sendTouch(1, cancelCoords[0], cancelCoords[1], event.getPointerId(i));
                }
                mNative.sendTouchFrame();
                return true;
        }
        return false;
    }
    
    private float[] convertToNativeCoords(float x, float y) {
        if (autoStretch) {
            float scaleX = (customScreenWidth > 0 && viewWidth > 0) ? 
                           (float)customScreenWidth / viewWidth : 1.0f;
            float scaleY = (customScreenHeight > 0 && viewHeight > 0) ? 
                           (float)customScreenHeight / viewHeight : 1.0f;
            return new float[]{x * scaleX, y * scaleY};
        } else {
            return new float[]{(x - surfaceOffsetX) / surfaceScale, (y - surfaceOffsetY) / surfaceScale};
        }
    }
    
    private float[] convertMouseToNative(float x, float y) {
        if (autoStretch) {
            float scaleX = (customScreenWidth > 0 && viewWidth > 0) ? 
                           (float)customScreenWidth / viewWidth : 1.0f;
            float scaleY = (customScreenHeight > 0 && viewHeight > 0) ? 
                           (float)customScreenHeight / viewHeight : 1.0f;
            return new float[]{x * scaleX, y * scaleY};
        } else {
            return new float[]{x / surfaceScale, y / surfaceScale};
        }
    }

}
