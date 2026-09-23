/*
 * SPDX-FileCopyrightText: 2026 kenway214
 * SPDX-License-Identifier: Apache-2.0
 */

package co.aospa.dolby.xiaomi.service

import android.app.ActivityManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.AudioPlaybackConfiguration
import android.os.Handler
import android.content.SharedPreferences
import android.os.IBinder
import android.util.Log
import co.aospa.dolby.xiaomi.R
import co.aospa.dolby.xiaomi.utils.ToastHelper
import co.aospa.dolby.xiaomi.DolbyConstants
import co.aospa.dolby.xiaomi.data.DeviceStateManager
import co.aospa.dolby.xiaomi.data.DolbyRepository

class DolbyEffectService : Service() {

    private val audioManager by lazy { getSystemService(AudioManager::class.java) }
    private val dolbyPrefs: SharedPreferences by lazy {
        getSharedPreferences("dolby_prefs", Context.MODE_PRIVATE)
    }
    private val isDeviceStateMemoryEnabled: Boolean
        get() = dolbyPrefs.getBoolean(DolbyConstants.PREF_DEVICE_STATE_MEMORY, false)
    private val handler = Handler()
    private lateinit var repository: DolbyRepository
    private lateinit var deviceStateManager: DeviceStateManager
    private var previousActiveDevice: AudioDeviceInfo? = null
    private var previousDeviceKey: String? = null

    private val audioDeviceCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<AudioDeviceInfo>) {
            Log.d(TAG, "Devices added: ${addedDevices.map { it.productName }}")
            scheduleDeviceChange()
        }

        override fun onAudioDevicesRemoved(removedDevices: Array<AudioDeviceInfo>) {
            Log.d(TAG, "Devices removed: ${removedDevices.map { it.productName }}")
            scheduleDeviceChange()
        }
    }

    private val deviceChange = Runnable { handleDeviceChange() }
    private val routeListener = AudioManager.OnDevicesForAttributesChangedListener { _, _ ->
        scheduleDeviceChange()
    }

    private fun scheduleDeviceChange() {
        handler.removeCallbacks(deviceChange)
        handler.postDelayed(deviceChange, ROUTE_SETTLE_MS)
    }

    private val playbackCallback = object : AudioManager.AudioPlaybackCallback() {
        override fun onPlaybackConfigChanged(configs: MutableList<AudioPlaybackConfiguration>?) {
            if (!handler.hasCallbacks(deviceChange)) scheduleDeviceChange()
            syncPlayback(configs ?: emptyList(), MAX_ATTACH_RETRIES)
        }
    }

    private var retriesLeft = 0
    private val retryAttachment = Runnable {
        syncPlayback(audioManager.activePlaybackConfigurations, retriesLeft)
    }
    // Param pushes go in before the engine wakes up and get tossed, so belt it again
    // once the audio thread's had a moment to come alive.
    private val pushSettings = Runnable { repository.reapplyToLiveEffects() }

    private fun syncPlayback(configs: List<AudioPlaybackConfiguration>, retries: Int) {
        handler.removeCallbacks(retryAttachment)
        if (!repository.syncSessions(configs) && retries > 0) {
            // A failed first attach needs another chance without pausing music.
            retriesLeft = retries - 1
            handler.postDelayed(retryAttachment, ATTACH_RETRY_MS)
        }
        handler.removeCallbacks(pushSettings)
        repository.reapplyToLiveEffects()
        handler.postDelayed(pushSettings, REAPPLY_MS)
    }

    // Disable Dolby when ViPER starts.
    private var viperUid = -1
    private val uidImportanceListener =
        ActivityManager.OnUidImportanceListener { uid, importance ->
            if (uid == viperUid &&
                importance <= ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND_SERVICE &&
                dolbyPrefs.getBoolean(DolbyConstants.PREF_ENABLE, false)
            ) {
                Log.d(TAG, "ViPER became active -> disabling Dolby (mutual exclusion)")
                repository.setDolbyEnabled(false)
            }
        }

    override fun onCreate() {
        super.onCreate()
        repository = DolbyRepository(this)
        deviceStateManager = DeviceStateManager(this)

        // If Dolby is already on at boot, make sure ViPER is not.
        if (dolbyPrefs.getBoolean(DolbyConstants.PREF_ENABLE, false)) {
            repository.forceStopViper()
        }
        registerViperWatch()
        val currentDevice = getCurrentOutputDevice()
        if (currentDevice != null) {
            previousActiveDevice = currentDevice
            previousDeviceKey = deviceStateManager.deviceKey(currentDevice)
            if (isDeviceStateMemoryEnabled) {
                val key = deviceStateManager.deviceKey(currentDevice)
                val restored = deviceStateManager.restoreSnapshot(key, repository)
                if (!restored) repository.applySavedState()
            } else {
                repository.applySavedState()
            }
        } else {
            repository.applySavedState()
        }

        audioManager.registerAudioDeviceCallback(audioDeviceCallback, handler)
        runCatching {
            audioManager.addOnDevicesForAttributesChangedListener(
                ATTRIBUTES_MEDIA, mainExecutor, routeListener)
        }.onFailure { Log.w(TAG, "Cannot observe media route changes", it) }
        audioManager.registerAudioPlaybackCallback(playbackCallback, handler)
        Log.d(TAG, "Dolby effect service created")
    }

    private fun handleDeviceChange() {
        // An unresolved/transient route must not save or restore another device's EQ.
        val newDevice = getCurrentOutputDevice() ?: return
        val newKey = deviceStateManager.deviceKey(newDevice)

        // Restore settings only when the active output changes.
        if (newKey == previousDeviceKey) {
            repository.updateSpeakerState()
            return
        }

        val oldKey = previousDeviceKey
        if (oldKey != null && isDeviceStateMemoryEnabled) {
            Log.d(TAG, "Saving snapshot for previous device: $oldKey")
            deviceStateManager.saveSnapshot(oldKey, repository)
        }

        previousActiveDevice = newDevice
        previousDeviceKey = newKey

        if (newDevice != null) {
            if (isDeviceStateMemoryEnabled) {
                Log.d(TAG, "Restoring snapshot for new device: $newKey")
                val restored = deviceStateManager.restoreSnapshot(newKey!!, repository)
                if (!restored) {
                    Log.d(TAG, "First time device, applying saved state as base")
                    repository.applySavedState()
                } else {
                    showDeviceProfileToast(newDevice)
                }
            } else {
                Log.d(TAG, "Device state memory disabled, applying saved state")
                repository.applySavedState()
            }
        } else {
            repository.updateSpeakerState()
            repository.applySavedState()
        }
    }

    private fun registerViperWatch() {
        try {
            viperUid = packageManager.getPackageUid(DolbyRepository.VIPER_PACKAGE, 0)
            val am = getSystemService(ActivityManager::class.java)
            am.addOnUidImportanceListener(
                uidImportanceListener,
                ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND_SERVICE
            )
            Log.d(TAG, "Watching ViPER uid=$viperUid for mutual exclusion")
        } catch (e: Exception) {
            // Skip the watch when ViPER is unavailable.
            Log.d(TAG, "registerViperWatch failed: ${e.message}")
        }
    }

    private fun showDeviceProfileToast(device: AudioDeviceInfo) {
        val profile = repository.getCurrentProfile().toString()
        val values = resources.getStringArray(R.array.dolby_profile_values)
        val names = resources.getStringArray(R.array.dolby_profile_entries)
        val name = names.getOrNull(values.indexOf(profile)) ?: getString(R.string.dolby_unknown)
        ToastHelper.showToast(this, getString(R.string.device_profile_restored_toast,
            deviceStateManager.deviceDisplayName(device), name))
    }

    private fun getCurrentOutputDevice(): AudioDeviceInfo? {
        val devices = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        return try {
            // Connected does not mean routed. Never guess by preferring Bluetooth:
            // this also avoids saving speaker EQ over an idle headset's snapshot.
            val routed = audioManager.getDevicesForAttributes(ATTRIBUTES_MEDIA).singleOrNull()
                ?: return null
            devices.filter {
                it.type == routed.type &&
                    (routed.address.isBlank() || it.address == routed.address)
            }.singleOrNull()
        } catch (e: Exception) {
            Log.d(TAG, "getDevicesForAttributes failed: ${e.message}")
            null
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        repository.applySavedState()
        syncPlayback(audioManager.activePlaybackConfigurations, MAX_ATTACH_RETRIES)
        return START_STICKY
    }

    override fun onDestroy() {
        super.onDestroy()
        if (isDeviceStateMemoryEnabled) {
            previousActiveDevice?.let { device ->
                val key = deviceStateManager.deviceKey(device)
                deviceStateManager.saveSnapshot(key, repository)
            }
        }
        audioManager.unregisterAudioDeviceCallback(audioDeviceCallback)
        runCatching { audioManager.removeOnDevicesForAttributesChangedListener(routeListener) }
        audioManager.unregisterAudioPlaybackCallback(playbackCallback)
        try {
            getSystemService(ActivityManager::class.java)
                .removeOnUidImportanceListener(uidImportanceListener)
        } catch (e: Exception) {
            Log.d(TAG, "removeOnUidImportanceListener failed: ${e.message}")
        }
        handler.removeCallbacksAndMessages(null)
        // The effects are process-wide; this service owns their lifetime.
        repository.shutdown()
        Log.d(TAG, "Dolby effect service destroyed")
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        private const val TAG = "DolbyEffectService"
        private const val MAX_ATTACH_RETRIES = 3
        private const val ATTACH_RETRY_MS = 500L
        private const val REAPPLY_MS = 700L
        private const val ROUTE_SETTLE_MS = 500L

        private val ATTRIBUTES_MEDIA = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .build()

        fun start(context: Context) {
            val intent = Intent(context, DolbyEffectService::class.java)
            context.startService(intent)
        }

        fun stop(context: Context) {
            val intent = Intent(context, DolbyEffectService::class.java)
            context.stopService(intent)
        }
    }
}
