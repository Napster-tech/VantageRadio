package com.example.vantageradioui.ui.main

import android.content.Context
import android.content.DialogInterface
import android.content.res.Resources
import android.graphics.Bitmap
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.InputMethodManager
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.example.vantageradioui.R
import com.example.vantageradioui.databinding.FragmentMainBinding
import com.google.zxing.BarcodeFormat
import com.google.zxing.WriterException
import com.journeyapps.barcodescanner.BarcodeEncoder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.*

class RadioControlFragment : Fragment() {

    private var _binding: FragmentMainBinding? = null
    private var lastRadioEnabledState: Boolean = false
    private var timerStarted: Boolean = false
    private var firstInit: Boolean = true
    private var firstInitUnconfigured: Boolean = true

    private val binding get() = _binding!!

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View? {
        _binding = FragmentMainBinding.inflate(inflater, container, false)
        super.onCreate(savedInstanceState)

        val prefs = requireActivity().getSharedPreferences("vantageradioprefs", Context.MODE_PRIVATE)
        val editor = prefs.edit()

        fun updateUI() {
            val state: String = getState()
            if (isModemConnected()) {
                binding.heartbeatStatus.text = "Drone Connected"
                binding.heartbeatStatus.setTextColor(resources.getColor(android.R.color.holo_green_dark, null))
            } else {
                binding.heartbeatStatus.text = "Drone Disconnected"
                binding.heartbeatStatus.setTextColor(resources.getColor(android.R.color.holo_red_dark, null))
            }

            if (state == "UNKNOWN" || state == "BOOTING" || state == "CONFIGURING" || state == "REMOVED") {
                binding.freqSpinner.isEnabled = false
                binding.bwSpinner.isEnabled = false
                binding.pairingViewButton.isEnabled = false
                binding.applySettings.isEnabled = false
                binding.scanChannelsButton.isEnabled=false
                firstInit = true
            } else {
                if (firstInit) {
                    val freqs: Array<Int> = getSupportedFreqs()
                    val bws: Array<Float> = getSupportedBWs()

                    val freqSpinnerArrayAdapter = ArrayAdapter(requireContext(), android.R.layout.simple_spinner_item, freqs)
                    val bwSpinnerArrayAdapter = ArrayAdapter(requireContext(), android.R.layout.simple_spinner_item, bws)

                    binding.freqSpinner.adapter = freqSpinnerArrayAdapter
                    binding.bwSpinner.adapter = bwSpinnerArrayAdapter

                    val currentFreq = getFreq()
                    val currentBW = getBW()
                    val currentPow = getPower()

                    val realNetwork = getNetworkID()
                    binding.networkID.setText(realNetwork)

                    binding.freqSpinner.setSelection(freqs.indexOf(currentFreq))
                    binding.bwSpinner.setSelection(bws.indexOf(currentBW))

                    val powerOptions = (7..30).toList()
                    val powerAdapter = ArrayAdapter(requireContext(), android.R.layout.simple_spinner_item, powerOptions)
                    powerAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
                    binding.powerSpinner.adapter = powerAdapter

                    val powerIndex = powerOptions.indexOf(currentPow)
                    if (powerIndex != -1) {
                        binding.powerSpinner.setSelection(powerIndex)
                    }

                    firstInit = false
                }

                binding.freqSpinner.isEnabled = true
                binding.bwSpinner.isEnabled = true
                binding.scanChannelsButton.isEnabled=true
                binding.pairingViewButton.isEnabled = true
                binding.applySettings.isEnabled = true
            }

            binding.radioStatus.text = stringFromJNI()
        }

        binding.applySettings.setOnClickListener {
            setFreqBW(
                binding.freqSpinner.selectedItem.toString().toInt(),
                binding.bwSpinner.selectedItem.toString().toFloat()
            )

            setNetworkName(binding.networkID.text.toString())
            setNetworkPassword(binding.networkPassword.text.toString())

            editor.putString("networkID", binding.networkID.text.toString())
            editor.putString("networkPass", binding.networkPassword.text.toString())
            editor.apply()

            applySettings()

            val powerValue = binding.powerSpinner.selectedItem as Int
            SetOutputPower(powerValue)
            Toast.makeText(requireContext(), "Power set to $powerValue", Toast.LENGTH_SHORT).show()
        }

        binding.enableRadioControlSwitch.setOnCheckedChangeListener { _, isChecked ->
            var radio: Int = if (binding.radioButtonMH.isChecked) 1 else if (binding.radioButtonSBS.isChecked) 3 else 0

            binding.networkPassword.visibility = if (radio == 3) View.INVISIBLE else View.VISIBLE
            binding.bwSpinner.visibility = if (radio == 3) View.INVISIBLE else View.VISIBLE
            binding.freqSpinner.visibility = if (radio == 3) View.INVISIBLE else View.VISIBLE

            binding.radioButtonMH.isEnabled = !isChecked
            binding.radioButtonSBS.isEnabled = !isChecked

            if (firstInitUnconfigured) {
                lastRadioEnabledState = prefs.getBoolean("radioEnabled", false)
                radio = prefs.getInt("radioSelection", 0)
                if (radio == 1) binding.radioButtonMH.isChecked = true
                else if (radio == 3) binding.radioButtonSBS.isChecked = true
                enableRadio(lastRadioEnabledState, radio)
            } else if (lastRadioEnabledState != isChecked) {
                enableRadio(isChecked, radio)
                editor.putBoolean("radioEnabled", isChecked)
                editor.putInt("radioSelection", radio)
                editor.apply()
            }

            lastRadioEnabledState = isChecked
            updateUI()
        }

        binding.closeQrCodeButton.setOnClickListener {
            binding.qrCode.visibility = View.INVISIBLE
            binding.closeQrCodeButton.visibility = View.INVISIBLE
            binding.pairingViewButton.visibility = View.VISIBLE
            binding.applySettings.visibility = View.VISIBLE
            binding.radioButtonMH.visibility = View.VISIBLE
            binding.radioButtonSBS.visibility = View.VISIBLE
        }

        binding.pairingViewButton.setOnClickListener {
            val width: Int = Resources.getSystem().getDisplayMetrics().widthPixels
            val height: Int = Resources.getSystem().getDisplayMetrics().heightPixels
            val bwString: String = if (binding.radioButtonMH.isChecked) "%.0f".format(getBW()) else "%.1f".format(getBW())
            val qrContent = "${binding.networkID.text}//${binding.networkPassword.text}//${getFreq()}//${getPower()}//$bwString//station//192.168.20.4"

            if (width > height) {
                binding.qrCode.setImageBitmap(generateQR(qrContent, height))
            } else {
                binding.qrCode.setImageBitmap(generateQR(qrContent, width))
            }

            binding.qrCode.visibility = View.VISIBLE
            binding.closeQrCodeButton.visibility = View.VISIBLE
            binding.pairingViewButton.visibility = View.INVISIBLE
            binding.applySettings.visibility = View.INVISIBLE
            binding.radioButtonMH.visibility = View.INVISIBLE
            binding.radioButtonSBS.visibility = View.INVISIBLE
        }

        // Scan Channels Button with loading animation and best frequency suggestion
        binding.scanChannelsButton.setOnClickListener {
            binding.progressBar.visibility = View.VISIBLE

            lifecycleScope.launch(Dispatchers.IO) {
                val sort = 1
                val count = 10
                val bw = getBW()
                val scannedChannels = scanChannels(sort, count, bw)

                withContext(Dispatchers.Main) {
                    binding.progressBar.visibility = View.GONE

                    if (scannedChannels.isNotEmpty()) {
                        val bestFrequency = scannedChannels.first() // Assuming first one is best, customize as needed
                        val bestFrequencyIndex = getSupportedFreqs().indexOf(bestFrequency)

                        // Show dialog to ask if user wants to apply the best frequency
                        AlertDialog.Builder(requireContext())
                            .setTitle("Best Frequency Found")
                            .setMessage("The best available frequency is $bestFrequency. Would you like to apply it?")
                            .setPositiveButton("Yes") { _, _ ->
                                // Set the frequency in spinner if user agrees
                                if (bestFrequencyIndex != -1) {
                                    binding.freqSpinner.setSelection(bestFrequencyIndex)
                                    Toast.makeText(requireContext(), "Frequency set to $bestFrequency", Toast.LENGTH_SHORT).show()
                                }
                            }
                            .setNegativeButton("No", null)
                            .show()
                    } else {
                        Toast.makeText(requireContext(), "No channels found.", Toast.LENGTH_SHORT).show()
                    }
                }
            }
        }

        if (!timerStarted) {
            Timer().scheduleAtFixedRate(object : TimerTask() {
                override fun run() {
                    Handler(Looper.getMainLooper()).post {
                        if (isAdded && isVisible) {
                            updateUI()
                        }
                    }
                }
            }, 500, 500)
            timerStarted = true
        }

        updateUI()

        binding.scrollView.setOnTouchListener { _, event ->
            if (event.action == MotionEvent.ACTION_DOWN) {
                clearFocusAndHideKeyboard()
            }
            false
        }

        binding.networkID.setOnFocusChangeListener { _, hasFocus ->
            if (!hasFocus) {
                clearFocusAndHideKeyboard()
            }
        }

        binding.networkPassword.setOnFocusChangeListener { _, hasFocus ->
            if (!hasFocus) {
                clearFocusAndHideKeyboard()
            }
        }

        return binding.root
    }

    private fun clearFocusAndHideKeyboard() {
        binding.networkID.clearFocus()
        binding.networkPassword.clearFocus()
        val imm = activity?.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.hideSoftInputFromWindow(binding.root.windowToken, 0)
    }

    external fun SetOutputPower(value: Int)
    external fun stringFromJNI(): String
    external fun enableRadio(enable: Boolean, type: Int): Int
    external fun getSupportedFreqs(): Array<Int>
    external fun getSupportedBWs(): Array<Float>
    external fun setFreqBW(freq: Int, bw: Float)
    external fun setNetworkName(name: String)
    external fun setNetworkPassword(pass: String)
    external fun getNetworkID(): String
    external fun getNetworkPassword(): String
    external fun applySettings()
    external fun getState(): String
    external fun getFreq(): Int
    external fun getBW(): Float
    external fun getPower(): Int
    external fun isModemConnected(): Boolean
    external fun scanChannels(sort: Int, count: Int, bw: Float): IntArray

    fun generateQR(content: String?, size: Int): Bitmap? {
        return try {
            val barcodeEncoder = BarcodeEncoder()
            barcodeEncoder.encodeBitmap(content, BarcodeFormat.QR_CODE, size, size)
        } catch (e: WriterException) {
            Log.e("generateQR()", "Issue!", e)
            null
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }
}
