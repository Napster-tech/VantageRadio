package com.example.vantageradioui

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.content.Intent
import android.content.Intent.FLAG_ACTIVITY_NEW_TASK
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.view.View
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.Toolbar
import androidx.viewpager.widget.ViewPager
import com.example.vantageradioui.databinding.ActivityMainBinding
import com.example.vantageradioui.ui.main.RadioControlFragment
import com.example.vantageradioui.ui.main.ScrollingFragment
import com.example.vantageradioui.ui.main.SectionsPagerAdapter
import com.example.vantageradioui.ui.main.SettingsFragment
import com.google.android.material.tabs.TabLayout

class VantageRadioControl : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    lateinit var notificationManager: NotificationManager
    lateinit var powerManager: PowerManager

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Inflate the layout using view binding
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // Enter full-screen immersive mode
        enterFullScreenMode()

        // Set up the toolbar
        val toolbar: Toolbar = findViewById(R.id.toolbar)
        setSupportActionBar(toolbar)

        supportActionBar?.setDisplayHomeAsUpEnabled(false)
        supportActionBar?.title = "" // Remove the title from the toolbar

        // Initialize the view pager and tabs
        val sectionsPagerAdapter = SectionsPagerAdapter(this, supportFragmentManager)
        val viewPager: ViewPager = binding.viewPager
        viewPager.adapter = sectionsPagerAdapter
        val tabs: TabLayout = binding.tabs
        tabs.setupWithViewPager(viewPager)

        // Create the fragments for the view pager
        sectionsPagerAdapter.radioControlFragment = RadioControlFragment()
        sectionsPagerAdapter.settingsFragment = SettingsFragment()
        sectionsPagerAdapter.scrollingFragment = ScrollingFragment()

        // Create the notification channel (API 26+)
        createNotificationChannel()

        // Handle battery optimization and overlay permissions for Android M+
        handlePermissions()
    }

    private fun enterFullScreenMode() {
        window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        or View.SYSTEM_UI_FLAG_FULLSCREEN
                        or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                )
    }

    private fun handlePermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            val packageName = this.packageName
            powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
            if (!powerManager.isIgnoringBatteryOptimizations(packageName)) {
                val intent = Intent().apply {
                    action = Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS
                    flags = FLAG_ACTIVITY_NEW_TASK
                    data = Uri.parse("package:$packageName")
                }
                startActivity(intent)
            }
            if (!Settings.canDrawOverlays(this)) {
                val intent = Intent().apply {
                    action = Settings.ACTION_MANAGE_OVERLAY_PERMISSION
                    flags = FLAG_ACTIVITY_NEW_TASK
                    data = Uri.parse("package:$packageName")
                }
                startActivity(intent)
            }
        }
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val name = getString(R.string.channel_id)
            val descriptionText = getString(R.string.channel_description)
            val importance = NotificationManager.IMPORTANCE_DEFAULT
            val channel = NotificationChannel("vantage_radio_notification", name, importance).apply {
                description = descriptionText
            }
            notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            notificationManager.createNotificationChannel(channel)
        }
    }

    companion object {
        init {
            System.loadLibrary("native-lib")
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            enterFullScreenMode()
        }
    }
}
