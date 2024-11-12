package com.example.vantageradioui.ui.main

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.fragment.app.Fragment
import com.example.vantageradioui.R
import kotlinx.android.synthetic.main.fragment_scrolling.*
import kotlinx.android.synthetic.main.fragment_scrolling.view.*
import java.util.*

class ScrollingFragment : Fragment() {
    private var textViewInitialized = false
    private var backlog = ""
    private val handler = Handler(Looper.getMainLooper())
    private val clearLogRunnable = Runnable { clearOldLogs() }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View? {
        val scrollView = inflater.inflate(R.layout.fragment_scrolling, container, false)

        if (!textViewInitialized) {
            Timer().scheduleAtFixedRate(object : TimerTask() {
                override fun run() {
                    Handler(Looper.getMainLooper()).post {
                        val logEntry: String = getRadioLog()
                        if (logEntry.isNotEmpty()) {
                            backlog += logEntry
                        }
                        if (isAdded && isVisible && userVisibleHint) {
                            logText.text = backlog
                        }
                    }
                }
            }, 200, 200)

            handler.postDelayed(clearLogRunnable, 10_000)  // Refresh every 10 seconds
            textViewInitialized = true
        }

        scrollView.clearLog.setOnClickListener {
            backlog = ""
            logText.text = backlog
        }

        return scrollView
    }

    private fun clearOldLogs() {
        // Here, we truncate the backlog to prevent excessive length
        if (backlog.length > 2000) {  // Example length limit
            backlog = backlog.takeLast(1000)  // Keep only the last 1000 characters
        }
        handler.postDelayed(clearLogRunnable, 10_000)  // Schedule next refresh
    }

    override fun onDestroyView() {
        super.onDestroyView()
        handler.removeCallbacks(clearLogRunnable)
    }

    external fun getRadioLog(): String
}
