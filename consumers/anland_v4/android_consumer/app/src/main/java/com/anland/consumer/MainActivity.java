package com.anland.consumer;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.hardware.display.DisplayManager;
import android.content.SharedPreferences;
import android.graphics.Color;
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
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;

import java.nio.charset.StandardCharsets;


public class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "Anland";

    private SurfaceView surfaceView;
    private boolean surfaceReady = false;
    private String mLastSentClip = null;
    private boolean mClipListening = false;
    private static final String PREFS_NAME = "anland_settings";
    private static final String KEY_BOUND_KEYCODE = "bound_keycode";
    private EditText hiddenInput;
    private InputMethodManager imm;
    private int mImeInset = -1;  // last IME bottom inset applied to the surface

    // evdev keycodes (linux/input-event-codes.h) for the editing keys a soft
    // keyboard emits as key events rather than text.
    private static final int EVDEV_ESC = 1;
    private static final int EVDEV_BACKSPACE = 14;
    private static final int EVDEV_TAB = 15;
    private static final int EVDEV_ENTER = 28;
    private static final int EVDEV_UP = 103;
    private static final int EVDEV_LEFT = 105;
    private static final int EVDEV_RIGHT = 106;
    private static final int EVDEV_DOWN = 108;
    private static final int EVDEV_DELETE = 111;

    static {
        System.loadLibrary("anland_consumer");
    }

    private native void nativeStart(Surface surface);
    private native void nativeStop();
    private native void nativeSendTouch(int action, float x, float y, int pointerId);
    private native void nativeSendTouchFrame();
    private native void nativeSendKey(int action, int keycode);
    private native void nativeSendMouseMotion(float x, float y, float dx, float dy);
    private native void nativeSendMouseButton(int button, boolean pressed);
    private native void nativeSendMouseScroll(int axis, float value);
    private native void nativeSetRefreshRate(float hz);
    private native void nativeSendClipboard(byte[] data);
    private native void nativeSendTextInput(byte[] data);
    // Called from native event thread to set clipboard text on Android
    public void nativeSetClipboardText(String text) {
        ClipboardManager cm = getSystemService(ClipboardManager.class);
        if (cm != null) {
            mLastSentClip = text;  // 记录，clipListener 回环时会比对跳过
            cm.setPrimaryClip(ClipData.newPlainText("anland", text));
        }
    }
    // Called from native C on exit_fallback to send initial clipboard sync
    public void nativeClipboardSync() {
        ClipboardManager cm = getSystemService(ClipboardManager.class);
        if (cm == null) return;
        ClipData clip = cm.getPrimaryClip();
        if (clip != null && clip.getItemCount() > 0) {
            CharSequence text = clip.getItemAt(0).getText();
            if (text != null) {
                mLastSentClip = text.toString();
                nativeSendClipboard(text.toString().getBytes(StandardCharsets.UTF_8));
            }
        }
    }

    private final ClipboardManager.OnPrimaryClipChangedListener clipListener =
        () -> pushClipboard();

    // Called from native C: true = register clip listener, false = unregister
    public void nativeClipListening(boolean enable) {
        ClipboardManager cm = getSystemService(ClipboardManager.class);
        if (cm == null) return;
        if (enable) {
            if (mClipListening) return;  // already registered
            cm.addPrimaryClipChangedListener(clipListener);
            mClipListening = true;
        } else {
            if (!mClipListening) return;  // not registered
            cm.removePrimaryClipChangedListener(clipListener);
            mClipListening = false;
        }
    }

    // Push clipboard only if content actually changed
    private void pushClipboard() {
        ClipboardManager cm = getSystemService(ClipboardManager.class);
        if (cm == null) return;
        ClipData clip = cm.getPrimaryClip();
        if (clip != null && clip.getItemCount() > 0) {
            CharSequence text = clip.getItemAt(0).getText();
            if (text != null) {
                String clipText = text.toString();
                if (!clipText.equals(mLastSentClip)) {
                    mLastSentClip = clipText;
                    nativeSendClipboard(clipText.getBytes(StandardCharsets.UTF_8));
                }
            }
        }
    }

    // Forwards the current display refresh rate to the daemon so KWin can repace
    // its RenderLoop. Re-fires on every onDisplayChanged (e.g. 60/90/120 switch).
    private final DisplayManager.DisplayListener displayListener =
        new DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int displayId) {}
            @Override public void onDisplayRemoved(int displayId) {}
            @Override public void onDisplayChanged(int displayId) {
                Display d = getDisplay();
                if (d != null && d.getDisplayId() == displayId)
                    pushRefreshRate();
            }
        };

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            pushClipboard();
        }
    }

    private void pushRefreshRate() {
        Display d = getDisplay();
        if (d != null)
            nativeSetRefreshRate(d.getRefreshRate());
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN);
        // Take over inset handling: the IME insets are dispatched to our
        // OnApplyWindowInsetsListener (so we can resize the surface) instead of
        // the system auto-panning the fullscreen window.
        getWindow().setDecorFitsSystemWindows(false);

        surfaceView = new SurfaceView(this);
        initHiddenInput();

        FrameLayout root = new FrameLayout(this);
        root.addView(surfaceView, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT));
        // 1x1 so the IME target never overlaps the surface and steals touches.
        root.addView(hiddenInput, new FrameLayout.LayoutParams(1, 1));
        setContentView(root);
        surfaceView.getHolder().addCallback(this);

        root.setOnApplyWindowInsetsListener((v, insets) -> {
            // When the IME hides by any means (toggle, system back, or the IME's
            // own close button), release the hidden input so its focus state
            // stays in sync — otherwise reopening needs a second press.
            if (!insets.isVisible(WindowInsets.Type.ime()))
                releaseHiddenInput();
            applyImeInset(insets);
            return v.onApplyWindowInsets(insets);
        });

        setupFullscreen();
        setupCursorHiding();
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

    @Override
    protected void onResume() {
        super.onResume();
        setupFullscreen();
        DisplayManager dm = getSystemService(DisplayManager.class);
        if (dm != null)
            dm.registerDisplayListener(displayListener, null);
        if (surfaceReady) {
            nativeStop();
            nativeStart(surfaceView.getHolder().getSurface());
            pushRefreshRate();
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        DisplayManager dm = getSystemService(DisplayManager.class);
        if (dm != null)
            dm.unregisterDisplayListener(displayListener);
        nativeStop();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Log.i(TAG, "surfaceChanged: " + width + "x" + height);
        surfaceReady = true;
        nativeStop();
        nativeStart(holder.getSurface());
        pushRefreshRate();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        nativeStop();
    }

    private void initHiddenInput() {
        imm = getSystemService(InputMethodManager.class);

        // Anonymous subclass so we can hand the IME our own InputConnection that
        // forwards text/keys to the remote in real time instead of buffering
        // them in an Editable that only flushes on Enter.
        hiddenInput = new EditText(this) {
            @Override
            public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
                super.onCreateInputConnection(outAttrs); // fills outAttrs only
                return new ForwardingInputConnection(this);
            }
        };
        hiddenInput.setBackgroundColor(Color.TRANSPARENT);
        hiddenInput.setCursorVisible(false);
        hiddenInput.setAlpha(0f);
        hiddenInput.setEnabled(false);          // 默认不拦截触摸
        hiddenInput.setFocusable(false);
        hiddenInput.setFocusableInTouchMode(false);
        hiddenInput.setClickable(false);
        hiddenInput.setLongClickable(false);
        // NO_ENTER_ACTION: deliver Enter as a key event we can forward, rather
        // than an editor action we'd have to swallow. NO_FULLSCREEN: never show
        // the landscape extract editor, which buffers text instead of sending it
        // live through our InputConnection.
        hiddenInput.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI
            | EditorInfo.IME_FLAG_NO_FULLSCREEN
            | EditorInfo.IME_FLAG_NO_ENTER_ACTION);
        hiddenInput.setInputType(android.text.InputType.TYPE_CLASS_TEXT
            | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            | android.text.InputType.TYPE_TEXT_VARIATION_NORMAL);
    }

    private void sendText(String text) {
        if (text.isEmpty()) return;
        nativeSendTextInput(text.getBytes(StandardCharsets.UTF_8));
    }

    private void tapKey(int evdevCode) {
        nativeSendKey(0, evdevCode);
        nativeSendKey(1, evdevCode);
    }

    // Map the few Android key codes a soft keyboard delivers as key events to the
    // evdev keycodes KWin expects. Returns 0 for keys we don't forward.
    private static int toEvdevKey(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER: return EVDEV_ENTER;
            case KeyEvent.KEYCODE_DEL:          return EVDEV_BACKSPACE;
            case KeyEvent.KEYCODE_FORWARD_DEL:  return EVDEV_DELETE;
            case KeyEvent.KEYCODE_TAB:          return EVDEV_TAB;
            case KeyEvent.KEYCODE_ESCAPE:       return EVDEV_ESC;
            case KeyEvent.KEYCODE_DPAD_LEFT:    return EVDEV_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT:   return EVDEV_RIGHT;
            case KeyEvent.KEYCODE_DPAD_UP:      return EVDEV_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN:    return EVDEV_DOWN;
            default:                            return 0;
        }
    }

    /*
     * Bridges the soft keyboard to the remote compositor in real time. Committed
     * text is forwarded as UTF-8 immediately; composing (preedit) text is
     * forwarded as it changes by diffing against what we already sent, so each
     * keystroke shows up live without waiting for Enter. Editing keys (Enter,
     * Backspace, ...) are forwarded as evdev key taps. We keep no Editable of our
     * own, so nothing accumulates between commits.
     */
    private final class ForwardingInputConnection extends BaseInputConnection {
        // What we have already forwarded for the in-progress composition.
        private final StringBuilder composing = new StringBuilder();

        ForwardingInputConnection(View target) {
            super(target, false);
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            final String s = text == null ? "" : text.toString();
            // Fast path: the commit just finalizes the current composition
            // unchanged — already forwarded, so only drop the tracker.
            if (composing.length() > 0 && composing.toString().equals(s)) {
                composing.setLength(0);
                return true;
            }
            eraseComposing();
            sendText(s);
            return true;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            replaceComposing(text == null ? "" : text.toString());
            return true;
        }

        @Override
        public boolean finishComposingText() {
            composing.setLength(0); // accepted as-is; keep what we forwarded
            return true;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            for (int i = 0; i < beforeLength; i++) {
                tapKey(EVDEV_BACKSPACE);
            }
            for (int i = 0; i < afterLength; i++) {
                tapKey(EVDEV_DELETE);
            }
            return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
            final int evdev = toEvdevKey(event.getKeyCode());
            if (evdev == 0) {
                return super.sendKeyEvent(event);
            }
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                nativeSendKey(0, evdev);
            } else if (event.getAction() == KeyEvent.ACTION_UP) {
                nativeSendKey(1, evdev);
            }
            return true;
        }

        // Forward only the delta between the previously-sent composition and the
        // new one: backspace the changed tail, then send the new tail.
        private void replaceComposing(String next) {
            final String prev = composing.toString();
            int prefix = 0;
            final int min = Math.min(prev.length(), next.length());
            while (prefix < min && prev.charAt(prefix) == next.charAt(prefix)) {
                prefix++;
            }
            if (prefix > 0 && Character.isHighSurrogate(prev.charAt(prefix - 1))) {
                prefix--; // never split a surrogate pair
            }
            final int erase = prev.codePointCount(prefix, prev.length());
            for (int i = 0; i < erase; i++) {
                tapKey(EVDEV_BACKSPACE);
            }
            if (prefix < next.length()) {
                sendText(next.substring(prefix));
            }
            composing.setLength(0);
            composing.append(next);
        }

        private void eraseComposing() {
            final int erase = composing.codePointCount(0, composing.length());
            for (int i = 0; i < erase; i++) {
                tapKey(EVDEV_BACKSPACE);
            }
            composing.setLength(0);
        }
    }

    // Shrink the surface to the area above the keyboard by giving it a bottom
    // margin equal to the IME height. The size change flows through
    // surfaceChanged -> nativeStart and the producer's resize path, so the
    // focused window relayouts into the upper region instead of hiding behind
    // the keyboard. Reset to 0 when the IME goes away.
    private void applyImeInset(WindowInsets insets) {
        int imeBottom = insets.getInsets(WindowInsets.Type.ime()).bottom;
        if (imeBottom == mImeInset) return;  // no change — skip surface restart
        mImeInset = imeBottom;
        FrameLayout.LayoutParams lp =
            (FrameLayout.LayoutParams) surfaceView.getLayoutParams();
        lp.bottomMargin = imeBottom;
        surfaceView.setLayoutParams(lp);
    }

    private boolean isImeVisible() {
        WindowInsets insets = getWindow().getDecorView().getRootWindowInsets();
        return insets != null && insets.isVisible(WindowInsets.Type.ime());
    }

    private void releaseHiddenInput() {
        if (!hiddenInput.isEnabled()) return;  // already released
        hiddenInput.clearFocus();
        hiddenInput.setFocusable(false);
        hiddenInput.setEnabled(false);
    }

    private void toggleKeyboard() {
        if (imm == null) imm = getSystemService(InputMethodManager.class);
        if (imm == null) return;
        if (isImeVisible()) {
            imm.hideSoftInputFromWindow(hiddenInput.getWindowToken(), 0);
            releaseHiddenInput();
        } else {
            hiddenInput.setEnabled(true);
            hiddenInput.setFocusable(true);
            hiddenInput.setFocusableInTouchMode(true);
            hiddenInput.requestFocus();
            imm.showSoftInput(hiddenInput, InputMethodManager.SHOW_IMPLICIT);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (isMouseEvent(event)) {
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
            if (action == MotionEvent.ACTION_HOVER_MOVE) {
                nativeSendMouseMotion(event.getX(), event.getY(),
                                      event.getAxisValue(MotionEvent.AXIS_RELATIVE_X),
                                      event.getAxisValue(MotionEvent.AXIS_RELATIVE_Y));
                return true;
            }
            if (action == MotionEvent.ACTION_SCROLL) {
                float vScroll = event.getAxisValue(MotionEvent.AXIS_VSCROLL);
                float hScroll = event.getAxisValue(MotionEvent.AXIS_HSCROLL);
                if (vScroll != 0)
                    nativeSendMouseScroll(0, -vScroll * 10);
                if (hScroll != 0)
                    nativeSendMouseScroll(1, hScroll * 10);
                return true;
            }
        }
        return super.onGenericMotionEvent(event);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (event.getRepeatCount() > 0)
            return true;

        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        int boundKeycode = prefs.getInt(KEY_BOUND_KEYCODE, -1);
        if (boundKeycode != -1 && keyCode == boundKeycode) {
            toggleKeyboard();
            return true;
        }

        int scanCode = event.getScanCode();
        if (scanCode != 0) {
            nativeSendKey(0, scanCode);
            return true;
        }
        return true;
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        int scanCode = event.getScanCode();
        if (scanCode != 0) {
            nativeSendKey(1, scanCode);
            return true;
        }
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

    private boolean handleMouseEvent(MotionEvent event) {
        float dx = 0f;
        float dy = 0f;
        if (event.getHistorySize() > 0) {
            int last = event.getHistorySize() - 1;
            dx = event.getX() - event.getHistoricalX(0, last);
            dy = event.getY() - event.getHistoricalY(0, last);
        }
        nativeSendMouseMotion(event.getX(), event.getY(), dx, dy);

        int currentBS = event.getButtonState();
        for (int[] btn : BUTTON_MAP) {
            boolean wasDown = (savedBS & btn[0]) != 0;
            boolean isDown  = (currentBS & btn[0]) != 0;
            if (wasDown != isDown)
                nativeSendMouseButton(btn[1], isDown);
        }
        savedBS = currentBS;
        return true;
    }

    private boolean handleTouchpadScroll(MotionEvent event) {
        if (event.getActionMasked() == MotionEvent.ACTION_MOVE) {
            float scrollX = event.getAxisValue(MotionEvent.AXIS_GESTURE_SCROLL_X_DISTANCE);
            float scrollY = event.getAxisValue(MotionEvent.AXIS_GESTURE_SCROLL_Y_DISTANCE);
            if (scrollY != 0)
                nativeSendMouseScroll(0, scrollY);
            if (scrollX != 0)
                nativeSendMouseScroll(1, -scrollX);
        }
        return true;
    }

    private boolean handleTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int pointerIdx = event.getActionIndex();
        int pointerId = event.getPointerId(pointerIdx);

        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                nativeSendTouch(0, event.getX(pointerIdx), event.getY(pointerIdx), pointerId);
                nativeSendTouchFrame();
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                nativeSendTouch(1, event.getX(pointerIdx), event.getY(pointerIdx), pointerId);
                nativeSendTouchFrame();
                return true;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); i++) {
                    nativeSendTouch(2, event.getX(i), event.getY(i), event.getPointerId(i));
                }
                nativeSendTouchFrame();
                return true;
            case MotionEvent.ACTION_CANCEL:
                for (int i = 0; i < event.getPointerCount(); i++) {
                    nativeSendTouch(1, event.getX(i), event.getY(i), event.getPointerId(i));
                }
                nativeSendTouchFrame();
                return true;
        }
        return false;
    }

}
