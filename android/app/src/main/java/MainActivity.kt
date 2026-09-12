package org.ok1iak.sdriak;

import android.app.NativeActivity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.DialogInterface;
import android.content.pm.PackageManager;
import android.hardware.usb.*;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.Manifest;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.window.OnBackInvokedDispatcher;
import android.view.HapticFeedbackConstants;
import android.view.Surface;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.KeyEvent;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.util.Log;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;

import androidx.core.app.ActivityCompat;
import androidx.core.content.IntentCompat;

import androidx.core.content.PermissionChecker;

import java.util.concurrent.LinkedBlockingQueue;
import java.io.*;

private val ACTION_USB_PERMISSION = "${BuildConfig.APPLICATION_ID}.USB_PERMISSION";

private val usbReceiver = object : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val activity = context as? MainActivity ?: return
        when (intent.action) {
            ACTION_USB_PERMISSION -> {
                synchronized(this) {
                    activity.SDR_device = IntentCompat.getParcelableExtra(intent, UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                        activity.SDR_conn = activity.usbManager?.openDevice(activity.SDR_device)
                        if (activity.SDR_conn != null && activity.SDR_device != null) {
                            activity.SDR_VID = activity.SDR_device!!.vendorId
                            activity.SDR_PID = activity.SDR_device!!.productId
                            activity.SDR_FD = activity.SDR_conn!!.fileDescriptor
                        }
                        activity.notifyUsbHotplugChanged()
                    }

                    activity.hideSystemBars()
                }
            }
            UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                val device = IntentCompat.getParcelableExtra(intent, UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                if (device != null) {
                    val manager = activity.usbManager
                    if (manager != null && manager.hasPermission(device)) {
                        activity.notifyUsbHotplugChanged()
                    }
                    else {
                        activity.requestUsbPermission(device)
                    }
                }
            }
            UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                val device = IntentCompat.getParcelableExtra(intent, UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                if (device != null && activity.SDR_device?.deviceId == device.deviceId) {
                    activity.SDR_conn?.close()
                    activity.SDR_conn = null
                    activity.SDR_device = null
                    activity.SDR_VID = -1
                    activity.SDR_PID = -1
                    activity.SDR_FD = -1
                }
                if (device != null) {
                    activity.detachRetainedUsbConnection(device.deviceName)
                    activity.notifyUsbHotplugChanged()
                }
            }
        }
    }
}
class MainActivity : NativeActivity() {
    private val TAG : String = "SDRIAK";
    public var usbManager : UsbManager? = null;
    public var SDR_device : UsbDevice? = null;
    public var SDR_conn : UsbDeviceConnection? = null;
    public var SDR_VID : Int = -1;
    public var SDR_PID : Int = -1;
    public var SDR_FD : Int = -1;

    // Sleep timer
    private lateinit var sleepTimer: SleepTimerManager;
    private var wakeLock: PowerManager.WakeLock? = null;
    private var usbPermissionIntent: PendingIntent? = null
    private var usbReceiverRegistered = false

    // Audio routing change detection
    private var lastKnownPreferredOutputId: Int = -1
    private val audioDeviceCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<AudioDeviceInfo>) = checkRoutingChanged()
        override fun onAudioDevicesRemoved(removedDevices: Array<AudioDeviceInfo>) = checkRoutingChanged()

        private fun checkRoutingChanged() {
            val newId = getPreferredAudioDeviceId(this@MainActivity, AudioManager.GET_DEVICES_OUTPUTS)
            if (newId != lastKnownPreferredOutputId) {
                lastKnownPreferredOutputId = newId
                notifyAudioRoutingChangedNative()
            }
        }
    }

    companion object {
        private const val QMX_USB_VID = 0x0483
        private const val QMX_USB_PID = 0xA34C
        private const val STORAGE_PERMISSION_REQUEST_CODE = 100
        private val openUsbConnections = HashMap<Int, UsbDeviceConnection>()
        private val openUsbDeviceNames = HashMap<String, Int>()

        init {
            System.loadLibrary("sdriak_core")
        }

        private fun isQmxUsbDevice(dev: UsbDevice?): Boolean {
            return dev != null && dev.vendorId == QMX_USB_VID && dev.productId == QMX_USB_PID
        }

        private fun isUsbAudioDevice(device: AudioDeviceInfo): Boolean {
            return device.type == AudioDeviceInfo.TYPE_USB_DEVICE ||
                device.type == AudioDeviceInfo.TYPE_USB_HEADSET ||
                device.type == AudioDeviceInfo.TYPE_USB_ACCESSORY
        }

        private fun containsQmxMarker(value: CharSequence?): Boolean {
            if (value == null) {
                return false
            }
            val text = value.toString().uppercase()
            return text.contains("QMX") || text.contains("QDX")
        }

        private fun isLikelyQmxAudioDevice(context: Context, device: AudioDeviceInfo, directionFlag: Int): Boolean {
            if (!isUsbAudioDevice(device)) {
                return false
            }
            val addressMatches = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                containsQmxMarker(device.address)
            }
            else {
               // On Android older than 9, we cannot confirm whether the audio USB device is a QRP Labs QMX or QDX product.
                // Thus we rather enable audio routing to USB in SDRIAK build for those old Android devices for now
                // and leave it to the user to disable USB audio routing in development mode.
                // Also the QMX source shows the recommendation in its UI page.
                false
            }
            if (containsQmxMarker(device.productName) || addressMatches) {
                return true
            }

            val usbManager = context.getSystemService(Context.USB_SERVICE) as? UsbManager ?: return false
            val qmxDevices = usbManager.deviceList.values.filter { isQmxUsbDevice(it) }
            if (qmxDevices.isEmpty()) {
                return false
            }

            for (usbDevice in qmxDevices) {
                if (containsQmxMarker(usbDevice.productName) ||
                    containsQmxMarker(usbDevice.manufacturerName) ||
                    containsQmxMarker(usbDevice.deviceName) ||
                    device.productName.toString().equals(usbDevice.productName ?: "", ignoreCase = true)
                ) {
                    return true
                }
            }

            val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager ?: return false
            val usbAudioCount = audioManager.getDevices(directionFlag).count { isUsbAudioDevice(it) }
            return usbAudioCount == 1
        }

        private fun outputRank(device: AudioDeviceInfo): Int {
            return when (device.type) {
                AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
                AudioDeviceInfo.TYPE_WIRED_HEADSET -> 0
                AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
                AudioDeviceInfo.TYPE_BLE_HEADSET,
                AudioDeviceInfo.TYPE_BLE_SPEAKER,
                AudioDeviceInfo.TYPE_BLE_BROADCAST -> 1
                AudioDeviceInfo.TYPE_BUILTIN_SPEAKER,
                AudioDeviceInfo.TYPE_BUILTIN_SPEAKER_SAFE -> 2
                AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> 3
                AudioDeviceInfo.TYPE_HDMI,
                AudioDeviceInfo.TYPE_HDMI_ARC,
                AudioDeviceInfo.TYPE_HDMI_EARC,
                AudioDeviceInfo.TYPE_LINE_ANALOG,
                AudioDeviceInfo.TYPE_LINE_DIGITAL,
                AudioDeviceInfo.TYPE_DOCK -> 4
                else -> if (isUsbAudioDevice(device)) 6 else 5
            }
        }

        private fun inputRank(device: AudioDeviceInfo): Int {
            return when (device.type) {
                AudioDeviceInfo.TYPE_WIRED_HEADSET -> 0
                AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
                AudioDeviceInfo.TYPE_BLE_HEADSET -> 1
                AudioDeviceInfo.TYPE_BUILTIN_MIC,
                AudioDeviceInfo.TYPE_FM_TUNER,
                AudioDeviceInfo.TYPE_TELEPHONY -> 2
                else -> if (isUsbAudioDevice(device)) 4 else 3
            }
        }

        private fun getPreferredAudioDeviceId(context: Context, directionFlag: Int): Int {
            val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager ?: return 0
            val devices = audioManager.getDevices(directionFlag)
            if (devices.isEmpty()) {
                return 0
            }

            val candidates = devices.filterNot { isLikelyQmxAudioDevice(context, it, directionFlag) }
            if (candidates.isEmpty()) {
                return 0
            }
            val ranked = if (directionFlag == AudioManager.GET_DEVICES_OUTPUTS) {
                candidates.minByOrNull(::outputRank)
            }
            else {
                candidates.minByOrNull(::inputRank)
            }
            return ranked?.id ?: 0
        }

        private fun getRetainedUsbConnectionFd(device: UsbDevice): Int? {
            synchronized(openUsbConnections) {
                val existingFd = openUsbDeviceNames[device.deviceName] ?: return null
                if (openUsbConnections.containsKey(existingFd)) {
                    return existingFd
                }
                openUsbDeviceNames.remove(device.deviceName)
                return null
            }
        }

        private fun retainUsbConnection(device: UsbDevice, conn: UsbDeviceConnection): Int {
            val fd = conn.fileDescriptor
            if (fd < 0) {
                conn.close()
                return -1
            }

            synchronized(openUsbConnections) {
                val oldFdForDevice = openUsbDeviceNames.remove(device.deviceName)
                if (oldFdForDevice != null) {
                    openUsbConnections.remove(oldFdForDevice)?.close()
                }

                val staleMappings = openUsbDeviceNames
                    .filterValues { it == fd }
                    .keys
                    .toList()
                for (deviceName in staleMappings) {
                    openUsbDeviceNames.remove(deviceName)
                }

                openUsbConnections.remove(fd)?.close()
                openUsbConnections[fd] = conn
                openUsbDeviceNames[device.deviceName] = fd
            }

            return fd
        }

        private fun closeRetainedUsbConnection(fd: Int) {
            synchronized(openUsbConnections) {
                openUsbConnections.remove(fd)?.close()
                val staleMappings = openUsbDeviceNames
                    .filterValues { it == fd }
                    .keys
                    .toList()
                for (deviceName in staleMappings) {
                    openUsbDeviceNames.remove(deviceName)
                }
            }
        }

        private fun detachRetainedUsbConnection(deviceName: String) {
            synchronized(openUsbConnections) {
                // Do NOT close the UsbDeviceConnection here: native code may
                // still stream on this fd through libusb_wrap_sys_device().
                // Closing the fd under libusb makes the DISCARDURB ioctls fail
                // with EBADF, so the cancelled transfers are never reaped and
                // the native teardown can only leak them (or, before the
                // libairspy/libairspyhf patches, crash in libusb_close()).
                // Kept open, the kernel reaps the URBs with NO_DEVICE and the
                // teardown completes cleanly; the connection is closed when
                // the native UsbDeviceLease releases it via
                // releaseOpenUsbDeviceHandle(). Only drop the name mapping so
                // a replugged device cannot be handed the dead connection.
                openUsbDeviceNames.remove(deviceName)
            }
        }

        private fun clearRetainedUsbConnections() {
            synchronized(openUsbConnections) {
                openUsbConnections.values.forEach { it.close() }
                openUsbConnections.clear()
                openUsbDeviceNames.clear()
            }
        }

        /**
         * Returns the file descriptor for a USB device matching the given VID and PID.
         * If multiple devices match, returns the first one found.
         * Returns -1 if not found or permission denied.
         * This can be called from JNI/native code.
         */
        @JvmStatic
        fun getOpenUsbDeviceHandleByVidPid(context: Context, vid: Int, pid: Int): String? {
            val usbManager = context.getSystemService(Context.USB_SERVICE) as? UsbManager ?: return null
            val devList = usbManager.deviceList
            for ((_, dev) in devList) {
                if (dev.vendorId == vid && dev.productId == pid) {
                    if (!usbManager.hasPermission(dev)) {
                        (context as? MainActivity)?.requestUsbPermission(dev)
                        return null
                    }
                    val existingFd = getRetainedUsbConnectionFd(dev)
                    if (existingFd != null) {
                        return "$existingFd|${dev.deviceName}"
                    }
                    val conn = usbManager.openDevice(dev) ?: return null
                    val fd = retainUsbConnection(dev, conn)
                    if (fd < 0) {
                        return null
                    }
                    return "$fd|${dev.deviceName}"
                }
            }
            return null
        }

        @JvmStatic
        fun releaseOpenUsbDeviceHandle(fd: Int) {
            closeRetainedUsbConnection(fd)
        }

        @JvmStatic
        fun getPreferredAudioOutputDeviceId(context: Context): Int {
            return getPreferredAudioDeviceId(context, AudioManager.GET_DEVICES_OUTPUTS)
        }
    }

    fun checkAndAsk(permission: String, requestCode: Int = 1) {
        if (PermissionChecker.checkSelfPermission(this, permission) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(permission), requestCode);
        }
    }

    private fun registerUsbReceiver() {
        if (usbReceiverRegistered) {
            return
        }
        val filter = IntentFilter().apply {
            addAction(ACTION_USB_PERMISSION)
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(usbReceiver, filter, Context.RECEIVER_EXPORTED)
        } else {
            registerReceiver(usbReceiver, filter)
        }
        usbReceiverRegistered = true
    }

    private fun unregisterUsbReceiver() {
        if (!usbReceiverRegistered) {
            return
        }
        try {
            unregisterReceiver(usbReceiver)
        }
        catch (_: IllegalArgumentException) {
        }
        usbReceiverRegistered = false
    }

    fun requestUsbPermission(device: UsbDevice) {
        val manager = usbManager ?: return
        if (manager.hasPermission(device)) {
            return
        }
        val permissionIntent = usbPermissionIntent ?: return
        manager.requestPermission(device, permissionIntent)
    }

    private fun requestUsbPermissionsForConnectedDevices() {
        val manager = usbManager ?: return
        for ((_, dev) in manager.deviceList) {
            if (manager.hasPermission(dev)) {
                notifyUsbHotplugChanged()
            }
            else {
                requestUsbPermission(dev)
            }
        }
    }

    fun detachRetainedUsbConnection(deviceName: String) {
        Companion.detachRetainedUsbConnection(deviceName)
    }

    private external fun notifyUsbHotplugChangedNative()
    private external fun notifyAudioRoutingChangedNative()

    fun notifyUsbHotplugChanged() {
        notifyUsbHotplugChangedNative()
    }

    private fun handleUsbAttachIntent(intent: Intent?) {
        if (intent?.action != UsbManager.ACTION_USB_DEVICE_ATTACHED) {
            return
        }
        val device = IntentCompat.getParcelableExtra(intent, UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        if (device != null) {
            val manager = usbManager
            if (manager != null && manager.hasPermission(device)) {
                notifyUsbHotplugChanged()
            }
            else {
                requestUsbPermission(device)
            }
        }
    }

    public fun hideSystemBars() {
        val decorView = getWindow().getDecorView();
        val uiOptions = View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
        decorView.setSystemUiVisibility(uiOptions);
    }

    public override fun onCreate(savedInstanceState: Bundle?) {
        // Force landscape orientation on phone-sized devices (smaller than large/tablet)
        val screenLayout = resources.configuration.screenLayout and Configuration.SCREENLAYOUT_SIZE_MASK
        if (screenLayout < Configuration.SCREENLAYOUT_SIZE_LARGE) {
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        }

        // Initialize sleep timer
        sleepTimer = SleepTimerManager(this);

        // Hide bars
        hideSystemBars();

        // Request storage permissions on API 23-28 (on API 29+ they are no-ops).
        // Native startup uses internal storage (getFilesDir), so the race with
        // permission grant is not harmful, but external-storage features (e.g.
        // recordings) may need these later.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            val storagePerms = arrayOf(
                Manifest.permission.WRITE_EXTERNAL_STORAGE,
                Manifest.permission.READ_EXTERNAL_STORAGE
            )
            val needed = storagePerms.filter {
                PermissionChecker.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
            }
            if (needed.isNotEmpty()) {
                ActivityCompat.requestPermissions(this, needed.toTypedArray(), STORAGE_PERMISSION_REQUEST_CODE)
            }
        }

        // Register events
        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager
        (getSystemService(Context.AUDIO_SERVICE) as AudioManager)
            .registerAudioDeviceCallback(audioDeviceCallback, null)
        val permissionRequestIntent = Intent(ACTION_USB_PERMISSION).setPackage(packageName)
        val pendingIntentFlags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE
        } else {
            PendingIntent.FLAG_UPDATE_CURRENT
        }
        usbPermissionIntent = PendingIntent.getBroadcast(this, 0, permissionRequestIntent, pendingIntentFlags)
        registerUsbReceiver()
        handleUsbAttachIntent(intent)

        // Get permission for all USB devices
        requestUsbPermissionsForConnectedDevices()

        // Ask for internet permission
        checkAndAsk(Manifest.permission.INTERNET);

        super.onCreate(savedInstanceState)

        // Back dispatch, new-style: with targetSdk 36, Android 16+ devices default
        // android:enableOnBackInvokedCallback to true, which stops delivering
        // KEYCODE_BACK to the native input queue entirely. Register a callback and
        // forward to native, where the same dismiss chain as the legacy
        // KEYCODE_BACK path runs. On Android 13-15 the attribute defaults to
        // false, this callback is never invoked, and the KEYCODE_BACK path in
        // backend.cpp handles Back instead — exactly one path fires per device.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT) {
                nativeOnBackInvoked()
            }
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == STORAGE_PERMISSION_REQUEST_CODE) {
            val denied = permissions.zip(grantResults.toList())
                .filter { it.second != PackageManager.PERMISSION_GRANTED }
                .map { it.first }
            if (denied.isNotEmpty()) {
                Log.w(TAG, "Storage permissions denied: $denied — some features may be unavailable")
            }
        }
    }

    public override fun onResume() {
        // Hide bars again
        hideSystemBars();
        super.onResume();
    }

    public override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleUsbAttachIntent(intent)
    }

    public override fun onDestroy() {
        sleepTimer.stop();
        (getSystemService(Context.AUDIO_SERVICE) as AudioManager)
            .unregisterAudioDeviceCallback(audioDeviceCallback)
        unregisterUsbReceiver()
        // Native source modules must cancel and reap libusb transfers before
        // the Java-owned file descriptors backing wrapped handles are closed.
        super.onDestroy();
        SDR_conn?.close()
        SDR_conn = null
        Companion.clearRetainedUsbConnections()
    }

    /**
     * Short haptic tick for UI feedback (e.g. a splitter drag handle engaging).
     * Callable from native code via JNI.
     */
    fun hapticTick() {
        runOnUiThread {
            window.decorView.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
        }
    }

    /**
     * Move the task to the background without finishing the activity, like a
     * Home press. Called from native code when Back is pressed with nothing
     * left to dismiss. Callable from native code via JNI.
     */
    fun moveAppToBack() {
        runOnUiThread { moveTaskToBack(true) }
    }

    /**
     * Fully exit: finish the activity and remove the task from the Recents
     * screen, so the app is visibly gone. Called from native code when the
     * user confirms the exit dialog. Callable from native code via JNI.
     */
    fun finishAppAndRemoveTask() {
        runOnUiThread { finishAndRemoveTask() }
    }

    // ── Sleep timer API ──────────────────────────────────────────────

    /**
     * Start the sleep timer.  Callable from native code via JNI.
     */
    fun startSleepTimer() {
        runOnUiThread { sleepTimer.start() }
    }

    /**
     * Stop the sleep timer and restore normal behavior.
     * Callable from native code via JNI.
     */
    fun stopSleepTimer() {
        runOnUiThread { sleepTimer.stop() }
    }

    /**
     * Called when the app window is destroyed (app goes to background).
     * Releases the wake lock immediately; the process will be suspended by Android anyway.
     * Callable from native code via JNI.
     */
    fun suspendSleepTimer() {
        runOnUiThread { sleepTimer.suspend() }
    }

    /**
     * Called when the app window is recreated (app returns to foreground).
     * Restarts the sleep timer from Active phase if the SDR source was running.
     * Callable from native code via JNI.
     */
    fun resumeSleepTimer() {
        runOnUiThread { sleepTimer.resume() }
    }

    /**
     * Configure the keep-alive mode and dim/blank thresholds in one call.
     * mode: 0=Disabled, 1=KeepAlive, 2=DimScreen, 3=DimAndBlank.
     * dimAfterSec / darkAfterSec: total seconds from timer start; darkAfterSec > dimAfterSec.
     * If the SDR source is running the change takes effect immediately.
     * Callable from native code via JNI.
     */
    fun setSleepTimerConfig(mode: Int, dimAfterSec: Int, darkAfterSec: Int) {
        val newMode = SleepTimerManager.Mode.entries.getOrElse(mode) { SleepTimerManager.Mode.DIM_AND_BLANK }
        runOnUiThread {
            sleepTimer.dimAfterMs  = dimAfterSec.toLong()  * 1000L
            sleepTimer.darkAfterMs = darkAfterSec.toLong() * 1000L
            sleepTimer.updateMode(newMode)
        }
    }

    /**
     * Reset the sleep timer to Active phase.
     * Called from native code on touch-to-wake or phone resume.
     */
    fun resetSleepToActive() {
        runOnUiThread { sleepTimer.resetToActive() }
    }

    /** Set screen brightness. -1f = system default, 0f = off, 0.01f = dim. */
    fun applySleepBrightness(brightness: Float) {
        runOnUiThread {
            val lp = window.attributes
            lp.screenBrightness = brightness
            window.attributes = lp
        }
    }

    /** Add FLAG_KEEP_SCREEN_ON and acquire a WakeLock to prevent the system from suspending. */
    fun applyKeepScreenOn() {
        runOnUiThread {
            Log.i("SleepTimer", "applyKeepScreenOn: setting FLAG_KEEP_SCREEN_ON and acquiring WakeLock")
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            if (wakeLock == null) {
                val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
                wakeLock = pm.newWakeLock(
                    PowerManager.PARTIAL_WAKE_LOCK,
                    "SDRIAK:SleepTimer"
                )
            }
            if (wakeLock?.isHeld == false) {
                wakeLock?.acquire(60L * 60 * 1000)  // 1 hour timeout as safety net
                Log.i("SleepTimer", "WakeLock acquired")
            }
        }
    }

    /** Remove FLAG_KEEP_SCREEN_ON and release the WakeLock — system idle timer resumes. */
    fun clearKeepScreenOn() {
        runOnUiThread {
            Log.i("SleepTimer", "clearKeepScreenOn: clearing FLAG_KEEP_SCREEN_ON and releasing WakeLock")
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            if (wakeLock?.isHeld == true) {
                wakeLock?.release()
                Log.i("SleepTimer", "WakeLock released")
            }
        }
    }

    /** Tell the native render loop to pause or resume. Zero-cost JNI: directly sets C++ flag. */
    fun setSleepRenderPaused(paused: Boolean) {
        nativeSetSleepRenderPaused(paused)
    }

    /** Mark the screen as dimmed (DIM or DARK phase). Guards touch/resume wake in C++. */
    fun setSleepScreenDimmed(dimmed: Boolean) {
        nativeSetSleepScreenDimmed(dimmed)
    }

    /** Native method that directly writes backend::sleepRenderPaused in C++. */
    // Back press delivered via OnBackInvokedCallback (Android 16+ path).
    private external fun nativeOnBackInvoked()

    private external fun nativeSetSleepRenderPaused(paused: Boolean)

    /** Native method that directly writes backend::sleepScreenDimmed in C++. */
    private external fun nativeSetSleepScreenDimmed(dimmed: Boolean)

    /**
     * Find the SurfaceView used by NativeActivity by walking the view hierarchy.
     */
    private fun findSurfaceView(view: View): SurfaceView? {
        if (view is SurfaceView) return view
        if (view is ViewGroup) {
            for (i in 0 until view.childCount) {
                val result = findSurfaceView(view.getChildAt(i))
                if (result != null) return result
            }
        }
        return null
    }

    /**
     * Hint the display to drop to 1 Hz refresh rate (LTPO panels).
     * Only effective on API 30+ (Android 11+); no-op on older devices.
     */
    fun setLowFrameRate() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            runOnUiThread {
                try {
                    val sv = findSurfaceView(window.decorView)
                    val surface = sv?.holder?.surface
                    if (surface != null && surface.isValid) {
                        surface.setFrameRate(
                            1.0f,
                            Surface.FRAME_RATE_COMPATIBILITY_DEFAULT
                        )
                        Log.i("SleepTimer", "setFrameRate(1.0) applied")
                    } else {
                        Log.w("SleepTimer", "setFrameRate: no valid Surface found")
                    }
                } catch (e: Exception) {
                    Log.w("SleepTimer", "setFrameRate failed: ${e.message}")
                }
            }
        }
    }

    /**
     * Restore the default frame rate (remove the 1 Hz hint).
     * Only effective on API 30+ (Android 11+); no-op on older devices.
     */
    fun restoreFrameRate() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            runOnUiThread {
                try {
                    val sv = findSurfaceView(window.decorView)
                    val surface = sv?.holder?.surface
                    if (surface != null && surface.isValid) {
                        surface.setFrameRate(
                            0.0f,
                            Surface.FRAME_RATE_COMPATIBILITY_DEFAULT
                        )
                        Log.i("SleepTimer", "setFrameRate(0.0) – restored default")
                    }
                } catch (e: Exception) {
                    Log.w("SleepTimer", "restoreFrameRate failed: ${e.message}")
                }
            }
        }
    }

    fun showSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager;
        inputMethodManager.showSoftInput(window.decorView, 0);
    }

    fun hideSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager;
        inputMethodManager.hideSoftInputFromWindow(window.decorView.windowToken, 0);
        hideSystemBars();
    }

    // Queue for the Unicode characters to be polled from native code (via pollUnicodeChar())
    private var unicodeCharacterQueue: LinkedBlockingQueue<Int> = LinkedBlockingQueue()

    // We assume dispatchKeyEvent() of the NativeActivity is actually called for every
    // KeyEvent and not consumed by any View before it reaches here
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN) {
            unicodeCharacterQueue.offer(event.getUnicodeChar(event.metaState))
        }
        return super.dispatchKeyEvent(event)
    }

    fun pollUnicodeChar(): Int {
        return unicodeCharacterQueue.poll() ?: 0
    }

    public fun createIfDoesntExist(path: String) {
        // This is a directory, create it in the filesystem
        var folder = File(path);
        var success = true;
        if (!folder.exists()) {
            success = folder.mkdirs();
        }
        if (!success) {
            Log.e(TAG, "Could not create folder with path " + path);
        }
    }

    public fun extractDir(aman: AssetManager, local: String, rsrc: String): Int {
        val flist = aman.list(rsrc) ?: return 0;
        var ecount = 0;
        for (fp in flist) {
            val lpath = local + "/" + fp;
            val rpath = rsrc + "/" + fp;

            Log.w(TAG, "Extracting '" + rpath + "' to '" + lpath + "'");

            // Create local path if non-existent
            createIfDoesntExist(local);
            
            // Create if directory
            val ext = extractDir(aman, lpath, rpath);

            // Extract if file
            if (ext == 0) {
                // This is a file, extract it
                val _os = FileOutputStream(lpath);
                val _is = aman.open(rpath);
                val ilen = _is.available();
                var fbuf = ByteArray(ilen);
                _is.read(fbuf, 0, ilen);
                _os.write(fbuf);
                _os.close();
                _is.close();
            }

            ecount++;
        }
        return ecount;
    }

    public fun getAppDir(): String {
        val fdir = getFilesDir().getAbsolutePath();

        // Extract all resources to the app directory
        val aman = getAssets();
        extractDir(aman, fdir + "/res", "res");
        createIfDoesntExist(fdir + "/modules");

        return fdir;
    }
}
