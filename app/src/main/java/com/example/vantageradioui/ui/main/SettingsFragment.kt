package com.example.vantageradioui.ui.main

import android.os.Bundle
import androidx.preference.PreferenceFragmentCompat
import com.example.vantageradioui.R

class SettingsFragment : PreferenceFragmentCompat() {

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        setPreferencesFromResource(R.xml.root_preferences, rootKey)
    }
}