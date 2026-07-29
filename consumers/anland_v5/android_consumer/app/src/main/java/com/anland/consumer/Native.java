package com.anland.consumer;

import android.view.Surface;

import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantReadWriteLock;

/**
 * JNI transport surface for the display consumer. All native methods bind by name to
 * {@code Java_com_anland_consumer_Native_*} in {@code jni/native_consumer.c}.
 *
 * The shared library is loaded by {@link MainActivity}'s static initializer (a
 * single {@code .so} backs this class, MainActivity and CameraServices).
 *
 * Instance-based: each consumer window owns its own {@code Native} (a native
 * consumer_state handle behind {@link #handle}), so multiple independent pipelines
 * coexist in one process. Every native method takes the handle as its first
 * argument; the public wrappers below thread it in. Call {@link #destroy()} when the
 * window is torn down.
 */
public final class Native {
    public static final int INPUT_GRAB_REASON_NONE = 0;
    public static final int INPUT_GRAB_REASON_INPUT_DIR_OPEN_FAILED = 1;
    public static final int INPUT_GRAB_REASON_DEVICE_LIMIT = 2;
    public static final int INPUT_GRAB_REASON_INVALID_EXIT_KEY = 3;
    public static final int INPUT_GRAB_REASON_EXIT_KEY_UNAVAILABLE = 4;
    public static final int INPUT_GRAB_REASON_EXIT_KEY_HELD = 5;
    public static final int INPUT_GRAB_REASON_NO_GRABBED_DEVICE = 6;
    public static final int INPUT_GRAB_REASON_GRAB_FAILED = 7;
    public static final int INPUT_GRAB_REASON_HOTPLUG_WATCH_FAILED = 8;
    public static final int INPUT_GRAB_REASON_STARTUP_IO_ERROR = 9;
    public static final int INPUT_GRAB_REASON_EXIT_KEY = 10;
    public static final int INPUT_GRAB_REASON_DEVICE_CHANGED = 11;
    public static final int INPUT_GRAB_REASON_DEVICE_LOST = 12;
    public static final int INPUT_GRAB_REASON_APP_STALLED = 13;
    public static final int INPUT_GRAB_REASON_PEER_CLOSED = 14;
    public static final int INPUT_GRAB_REASON_STREAM_IO_ERROR = 15;
    public static final int INPUT_GRAB_REASON_INPUT_BUSY = 16;

    private long handle;
    private final ReentrantReadWriteLock callLock = new ReentrantReadWriteLock(true);
    private final Lock sharedCall = callLock.readLock();
    private final Lock exclusiveCall = callLock.writeLock();

    /** Hot input must never wait behind lifecycle joins. Respect an already queued
     * writer as well as the held write lock, so a high-rate pointer stream cannot
     * repeatedly barge ahead of stop/destroy and keep immersion in STOPPING. */
    private boolean trySharedCall() {
        if (callLock.hasQueuedThreads())
            return false;
        return sharedCall.tryLock();
    }

    public Native() {
        handle = nativeCreate();
    }

    /** Release the native instance. Idempotent; the object is unusable afterwards. */
    public void destroy() {
        exclusiveCall.lock();
        try {
            if (handle != 0) {
                nativeDestroy(handle);
                handle = 0;
            }
        } finally {
            exclusiveCall.unlock();
        }
    }

    // ---- instance API (delegates to the handle-taking natives) ----

    public void configure(String socketPath, boolean useRoot, String helperPath, String bridgePath) {
        exclusiveCall.lock();
        try { if (handle != 0) nativeConfigure(handle, socketPath, useRoot, helperPath, bridgePath); }
        finally { exclusiveCall.unlock(); }
    }
    public void start(Surface surface, Object clipboardTarget, Object activityTarget) {
        exclusiveCall.lock();
        try { if (handle != 0) nativeStart(handle, surface, clipboardTarget, activityTarget); }
        finally { exclusiveCall.unlock(); }
    }
    public void stop() {
        exclusiveCall.lock();
        try { if (handle != 0) nativeStop(handle); }
        finally { exclusiveCall.unlock(); }
    }
    /** Mark this instance focused: its camera client receives real frames, others blank. */
    public void setFocused(boolean focused) {
        if (!trySharedCall()) return;
        try { if (handle != 0) nativeSetFocused(handle, focused); }
        finally { sharedCall.unlock(); }
    }
    public void setCustomResolution(int width, int height) {
        exclusiveCall.lock();
        try { if (handle != 0) nativeSetCustomResolution(handle, width, height); }
        finally { exclusiveCall.unlock(); }
    }
    public void sendTouch(int action, float x, float y, int pointerId) {
        if (!trySharedCall()) return;
        try { if (handle != 0) nativeSendTouch(handle, action, x, y, pointerId); }
        finally { sharedCall.unlock(); }
    }
    public void sendTouchFrame() {
        if (!trySharedCall()) return;
        try { if (handle != 0) nativeSendTouchFrame(handle); }
        finally { sharedCall.unlock(); }
    }
    public void sendKey(int action, int keycode) {
        if (!trySharedCall()) return;
        try { if (handle != 0) nativeSendKey(handle, action, keycode); }
        finally { sharedCall.unlock(); }
    }
    public void sendMouseMotion(float x, float y, float dx, float dy) {
        if (!trySharedCall()) return;
        try { if (handle != 0) nativeSendMouseMotion(handle, x, y, dx, dy); }
        finally { sharedCall.unlock(); }
    }
    public void sendMouseButton(int button, boolean pressed) {
        if (!trySharedCall()) return;
        try { if (handle != 0) nativeSendMouseButton(handle, button, pressed); }
        finally { sharedCall.unlock(); }
    }
    public void sendMouseScroll(int axis, float value) {
        if (!trySharedCall()) return;
        try { if (handle != 0) nativeSendMouseScroll(handle, axis, value); }
        finally { sharedCall.unlock(); }
    }
    public void setRefreshRate(float hz) {
        if (!trySharedCall()) return;
        try { if (handle != 0) nativeSetRefreshRate(handle, hz); }
        finally { sharedCall.unlock(); }
    }
    public void sendClipboard(byte[] data) {
        if (!trySharedCall()) return;
        try { if (handle != 0) nativeSendClipboard(handle, data); }
        finally { sharedCall.unlock(); }
    }
    public void sendTextInput(byte[] data) {
        if (!trySharedCall()) return;
        try { if (handle != 0) nativeSendTextInput(handle, data); }
        finally { sharedCall.unlock(); }
    }
    public void setMicEnabled(boolean enabled) {
        if (!trySharedCall()) return;
        try { if (handle != 0) nativeSetMicEnabled(handle, enabled); }
        finally { sharedCall.unlock(); }
    }
    public void setAudioLatency(int speakerMs, int micMs) {
        if (!trySharedCall()) return;
        try { if (handle != 0) nativeSetAudioLatency(handle, speakerMs, micMs); }
        finally { sharedCall.unlock(); }
    }
    /** Immersive full-input grab: root helper EVIOCGRABs the touchscreen + keys and
     * streams raw evdev here, so Android sees no input. See input_grab.h.
     * exitKeyCode is the evdev key that toggles immersion; the helper watches for it
     * root-side, so the release works even if this process is wedged. touchpadMode is
     * a session snapshot: finger-touch frames are returned to MainActivity for the
     * existing relative gesture recognizer instead of being injected absolutely. */
    public void startInputGrab(String helperPath, String bridgePath, int rotation,
                               int exitKeyCode, long sessionId,
                               boolean touchpadMode) {
        exclusiveCall.lock();
        try {
            if (handle != 0)
                nativeStartInputGrab(handle, helperPath, bridgePath, rotation,
                        exitKeyCode, sessionId, touchpadMode);
        } finally {
            exclusiveCall.unlock();
        }
    }
    public void stopInputGrab() {
        exclusiveCall.lock();
        try { if (handle != 0) nativeStopInputGrab(handle); }
        finally { exclusiveCall.unlock(); }
    }
    /** Update the grab touch rotation (Surface.ROTATION_* 0..3) live on display rotation. */
    public void setGrabRotation(int rotation) {
        if (!trySharedCall()) return;
        try { if (handle != 0) nativeSetGrabRotation(handle, rotation); }
        finally { sharedCall.unlock(); }
    }

    // ---- native handle lifecycle + handle-taking entry points ----

    private static native long nativeCreate();
    private static native void nativeDestroy(long handle);
    private static native void nativeSetFocused(long handle, boolean focused);

    private static native void nativeConfigure(long handle, String socketPath, boolean useRoot,
                                               String helperPath, String bridgePath);

    // With static natives there is no `thiz`, so native is handed the object it
    // calls back into (the Clipboard hosting nativeSetClipboardText /
    // nativeClipListening / nativeClipboardSync). It is stored per-instance as the
    // global ref used by the event thread's clipboard callbacks.
    // activityTarget is the owning MainActivity; native calls its onFallback() when the
    // display lib drops the connection (see on_fallback in native_consumer.c).
    private static native void nativeStart(long handle, Surface surface, Object clipboardTarget,
                                           Object activityTarget);
    private static native void nativeStop(long handle);
    private static native void nativeSetCustomResolution(long handle, int width, int height);
    private static native void nativeSendTouch(long handle, int action, float x, float y, int pointerId);
    private static native void nativeSendTouchFrame(long handle);
    private static native void nativeSendKey(long handle, int action, int keycode);
    private static native void nativeSendMouseMotion(long handle, float x, float y, float dx, float dy);
    private static native void nativeSendMouseButton(long handle, int button, boolean pressed);
    private static native void nativeSendMouseScroll(long handle, int axis, float value);
    private static native void nativeSetRefreshRate(long handle, float hz);
    private static native void nativeSendClipboard(long handle, byte[] data);
    private static native void nativeSendTextInput(long handle, byte[] data);
    private static native void nativeSetMicEnabled(long handle, boolean enabled);
    private static native void nativeSetAudioLatency(long handle, int speakerMs, int micMs);
    private static native void nativeStartInputGrab(long handle, String helperPath,
            String bridgePath, int rotation, int exitKeyCode, long sessionId,
            boolean touchpadMode);
    private static native void nativeStopInputGrab(long handle);
    private static native void nativeSetGrabRotation(long handle, int rotation);
}
