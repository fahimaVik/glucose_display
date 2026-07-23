package com.example.glucosewifi

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.google.android.material.card.MaterialCardView
import org.json.JSONArray
import org.json.JSONObject
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

// V2 setup wizard. Talks to the user's Nightscout only to validate the URL and
// token (Step 1), then provisions the display over its hotspot (Steps 2-3) with
// one POST. Units and colour thresholds are pulled from Nightscout, not typed.
//
// No third-party libraries: plain HttpURLConnection on a background thread,
// results posted to the UI thread. Connection to the device uses the manual
// "join GlucoseSetup" flow at this stage; one-tap connect comes in Stage 3.
class MainActivity : AppCompatActivity() {

    private val main = Handler(Looper.getMainLooper())

    // Validated Nightscout config, carried from Step 1 into the provision call.
    private var nsHost = ""
    private var nsToken = ""
    private var units = 0
    private var thresholds: JSONObject? = null

    private var selectedSsid = ""

    private lateinit var steps: Map<String, View>
    private lateinit var subtitle: TextView
    private lateinit var log: TextView
    private var current = "home"

    // Which step the Back button / back gesture returns to.
    private val backTo = mapOf(
        "ns" to "home", "connect" to "ns", "wifi" to "connect", "done" to "home"
    )

    private fun deviceBase(): String {
        val addr = findViewById<EditText>(R.id.deviceAddr).text.toString().trim()
        return "http://" + (if (addr.isEmpty()) "192.168.4.1" else addr)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        subtitle = findViewById(R.id.subtitle)
        log = findViewById(R.id.log)

        steps = mapOf(
            "home"    to findViewById(R.id.stepHome),
            "ns"      to findViewById(R.id.stepNs),
            "connect" to findViewById(R.id.stepConnect),
            "wifi"    to findViewById(R.id.stepWifi),
            "sending" to findViewById(R.id.stepSending),
            "done"    to findViewById(R.id.stepDone)
        )

        findViewById<Button>(R.id.homeSetup).setOnClickListener { show("ns") }
        findViewById<Button>(R.id.homeRefresh).setOnClickListener { refreshHome() }
        findViewById<Button>(R.id.nsOpen).setOnClickListener { openNightscout() }
        findViewById<Button>(R.id.nsCheck).setOnClickListener { checkNightscout() }
        findViewById<Button>(R.id.nsNext).setOnClickListener { show("connect") }
        findViewById<Button>(R.id.nsBack).setOnClickListener { goBack() }
        findViewById<Button>(R.id.connectFind).setOnClickListener { findDevice() }
        findViewById<Button>(R.id.connectNext).setOnClickListener { show("wifi") }
        findViewById<Button>(R.id.connectBack).setOnClickListener { goBack() }
        findViewById<Button>(R.id.wifiScan).setOnClickListener { scanWifi() }
        findViewById<Button>(R.id.wifiSend).setOnClickListener { provision() }
        findViewById<Button>(R.id.wifiBack).setOnClickListener { goBack() }
        findViewById<Button>(R.id.doneHome).setOnClickListener { show("home"); refreshHome() }

        show("home")
        refreshHome()
    }

    private fun show(step: String) {
        current = step
        for ((k, v) in steps) v.visibility = if (k == step) View.VISIBLE else View.GONE
        subtitle.text = if (step == "home") "Manage your display" else "Setting up your display"
        setLog("")
    }

    private fun goBack() {
        val prev = backTo[current]
        if (prev != null) { show(prev); if (prev == "home") refreshHome() }
    }

    // Back gesture / button steps through the wizard instead of quitting.
    @Deprecated("kept for API < 33")
    override fun onBackPressed() {
        if (backTo.containsKey(current)) goBack() else super.onBackPressed()
    }

    // Opens the entered site in a browser so the user can create/copy a token.
    private fun openNightscout() {
        val host = nsHostOf(findViewById<EditText>(R.id.nsUrl).text.toString())
        if (host.isEmpty()) {
            Toast.makeText(this, "Type your site URL in the field above first, then tap Open.",
                Toast.LENGTH_LONG).show()
            return
        }
        try {
            val i = Intent(Intent.ACTION_VIEW, Uri.parse("https://$host"))
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            startActivity(i)
        } catch (e: Exception) {
            Toast.makeText(this, "Could not open a browser: ${e.message}", Toast.LENGTH_LONG).show()
        }
    }

    private fun setLog(s: String) { log.text = s }

    private fun col(id: Int) = ContextCompat.getColor(this, id)

    // Paints a status card (day/night colour resources) and shows it.
    private fun statusCard(cardId: Int, textId: Int, bg: Int, fg: Int, msg: String) {
        val card = findViewById<MaterialCardView>(cardId)
        val tv = findViewById<TextView>(textId)
        card.setCardBackgroundColor(col(bg))
        tv.setTextColor(col(fg))
        tv.text = msg
        card.visibility = View.VISIBLE
    }

    // --- networking ---------------------------------------------------------

    private fun http(method: String, url: String, body: String?, done: (Int, String) -> Unit) {
        Thread {
            var code = -1
            var resp = ""
            try {
                val c = URL(url).openConnection() as HttpURLConnection
                c.requestMethod = method
                c.connectTimeout = 6000
                c.readTimeout = 12000        // Northflank can be slow to wake
                if (body != null) {
                    c.doOutput = true
                    c.setRequestProperty("Content-Type", "application/json")
                    OutputStreamWriter(c.outputStream).use { it.write(body) }
                }
                code = c.responseCode
                val stream = if (code in 200..299) c.inputStream else c.errorStream
                resp = stream?.bufferedReader()?.readText() ?: ""
                c.disconnect()
            } catch (e: Exception) {
                resp = e.message ?: "connection error"
            }
            main.post { done(code, resp) }
        }.start()
    }

    private fun nsHostOf(raw: String): String {
        var h = raw.trim()
        if (h.startsWith("https://")) h = h.substring(8)
        else if (h.startsWith("http://")) h = h.substring(7)
        val slash = h.indexOf('/')
        if (slash >= 0) h = h.substring(0, slash)
        return h
    }

    // --- Step 1: Nightscout -------------------------------------------------

    private fun checkNightscout() {
        val rawUrl = findViewById<EditText>(R.id.nsUrl).text.toString()
        val tokenField = findViewById<EditText>(R.id.nsToken)

        // If a full URL with ?token=... was pasted, lift the token out of it.
        val m = Regex("[?&]token=([^&\\s]+)").find(rawUrl)
        if (m != null && tokenField.text.toString().isBlank()) {
            tokenField.setText(m.groupValues[1])
        }

        val host = nsHostOf(rawUrl)
        val token = tokenField.text.toString().trim()

        if (host.isEmpty() || token.isEmpty()) {
            nsError("Enter both the site URL and the token."); return
        }

        statusCard(R.id.nsResultCard, R.id.nsResult, R.color.neutral_bg, R.color.neutral_fg,
            "Contacting Nightscout…")

        val tokenEnc = URLEncoder.encode(token, "UTF-8")
        // status.json gives units + thresholds + title; entries confirms the
        // token can actually read glucose (the real device operation).
        http("GET", "https://$host/api/v1/status.json?token=$tokenEnc", null) { sCode, sBody ->
            if (sCode != 200) {
                nsError(if (sCode < 0) "Site not reachable — check the URL."
                        else "Nightscout returned HTTP $sCode.")
                return@http
            }
            var title = "your Nightscout"
            try {
                val doc = JSONObject(sBody)
                val settings = doc.optJSONObject("settings")
                if (settings != null) {
                    units = if (settings.optString("units", "mg/dl").startsWith("mmol")) 1 else 0
                    val t = settings.optJSONObject("thresholds")
                    if (t != null) {
                        thresholds = JSONObject()
                            .put("urgent_high", t.optInt("bgHigh", 250))
                            .put("high",        t.optInt("bgTargetTop", 180))
                            .put("low",         t.optInt("bgTargetBottom", 80))
                            .put("urgent_low",  t.optInt("bgLow", 55))
                    }
                    val ct = settings.optString("customTitle", "")
                    if (ct.isNotEmpty()) title = ct
                }
            } catch (e: Exception) { /* keep defaults */ }

            // now confirm the token reads glucose
            http("GET", "https://$host/api/v1/entries.json?count=1&token=$tokenEnc", null) { eCode, eBody ->
                if (eCode == 401 || eCode == 403) { nsError("Token rejected — check the token."); return@http }
                if (eCode != 200) { nsError("Could not read glucose (HTTP $eCode)."); return@http }

                var latest = ""
                try {
                    val arr = JSONArray(eBody)
                    if (arr.length() > 0) latest = " Latest: ${arr.getJSONObject(0).optInt("sgv")} mg/dL."
                } catch (e: Exception) {}

                nsHost = host
                nsToken = token
                val unitTxt = if (units == 1) "mmol/L" else "mg/dL"
                statusCard(R.id.nsResultCard, R.id.nsResult, R.color.ok_bg, R.color.ok_fg,
                    "Connected to $title.$latest\nUnits ($unitTxt) and range pulled automatically.")
                findViewById<Button>(R.id.nsNext).isEnabled = true
            }
        }
    }

    private fun nsError(msg: String) {
        statusCard(R.id.nsResultCard, R.id.nsResult, R.color.err_bg, R.color.err_fg, msg)
        findViewById<Button>(R.id.nsNext).isEnabled = false
    }

    // --- Step 2: connect to device -----------------------------------------

    private fun findDevice() {
        statusCard(R.id.connectStatusCard, R.id.connectStatus, R.color.neutral_bg, R.color.neutral_fg,
            "Looking for the display…")

        http("GET", deviceBase() + "/api/status", null) { code, body ->
            if (code != 200) {
                statusCard(R.id.connectStatusCard, R.id.connectStatus, R.color.err_bg, R.color.err_fg,
                    "Not found. On GlucoseSetup? Mobile data off?")
                return@http
            }
            statusCard(R.id.connectStatusCard, R.id.connectStatus, R.color.ok_bg, R.color.ok_fg,
                "Display found.")
            findViewById<Button>(R.id.connectNext).isEnabled = true
        }
    }

    // --- Step 3: pick Wi-Fi -------------------------------------------------

    private fun scanWifi() {
        setLog("Scanning...")
        http("GET", deviceBase() + "/api/scan", null) { code, body ->
            val list = findViewById<LinearLayout>(R.id.wifiList)
            list.removeAllViews()
            if (code != 200) { setLog("Scan failed: $body"); return@http }
            val arr = try { JSONArray(body) } catch (e: Exception) { JSONArray() }
            if (arr.length() == 0) { setLog("No networks found. Try again."); return@http }
            selectedSsid = ""
            for (i in 0 until arr.length()) {
                val ssid = arr.getString(i)
                val row = TextView(this)
                row.text = ssid
                row.textSize = 16f
                row.setPadding(28, 34, 28, 34)
                row.setBackgroundColor(col(R.color.row_bg))
                row.setTextColor(col(R.color.neutral_fg))
                row.setOnClickListener {
                    selectedSsid = ssid
                    for (j in 0 until list.childCount) {
                        val v = list.getChildAt(j) as TextView
                        v.setBackgroundColor(col(R.color.row_bg))
                        v.setTextColor(col(R.color.neutral_fg))
                    }
                    row.setBackgroundColor(col(R.color.row_sel))
                    row.setTextColor(col(R.color.row_sel_text))
                }
                val lp = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
                lp.setMargins(0, 0, 0, 8)
                row.layoutParams = lp
                list.addView(row)
            }
            setLog("Tap your network, then enter its password.")
        }
    }

    private fun provision() {
        if (selectedSsid.isEmpty()) { setLog("Pick a Wi-Fi network first."); return }
        if (nsHost.isEmpty()) { setLog("Nightscout not validated — go back to Step 1."); return }

        val pass = findViewById<EditText>(R.id.wifiPass).text.toString()
        val body = JSONObject()
            .put("wifi_ssid", selectedSsid)
            .put("wifi_pass", pass)
            .put("ns_url", nsHost)
            .put("ns_token", nsToken)
            .put("units", units)
        thresholds?.let { body.put("thresholds", it) }

        show("sending")
        http("POST", deviceBase() + "/api/provision", body.toString()) { code, resp ->
            if (code != 200) {
                findViewById<TextView>(R.id.sendStatus).text = "Setup failed (HTTP $code): $resp"
                return@http
            }
            findViewById<TextView>(R.id.sendStatus).text =
                "Sent. The display is joining $selectedSsid and Nightscout.\n" +
                "Reconnect your phone to your home Wi-Fi, then tap below."
            // Give the device time to switch, then move to confirmation.
            main.postDelayed({ show("done"); confirm() }, 6000)
        }
    }

    // --- Step 4: confirm ----------------------------------------------------

    private fun confirm() {
        val done = findViewById<TextView>(R.id.doneStatus)
        done.text = "Checking the display..."
        http("GET", deviceBase() + "/api/status", null) { code, body ->
            if (code != 200) {
                done.text = "Setup sent. Reconnect your phone to your home Wi-Fi, " +
                        "then open this app to confirm."
                return@http
            }
            renderStatusInto(done, body, "✅ ")
        }
    }

    // --- Home ---------------------------------------------------------------

    private fun refreshHome() {
        val home = findViewById<TextView>(R.id.homeStatus)
        home.text = "Checking device..."
        http("GET", deviceBase() + "/api/status", null) { code, body ->
            if (code != 200) {
                home.text = "Display not reachable.\nSet the device address, join its Wi-Fi, " +
                        "or tap Set up to start."
                return@http
            }
            renderStatusInto(home, body, "")
        }
    }

    private fun renderStatusInto(tv: TextView, body: String, prefix: String) {
        try {
            val d = JSONObject(body)
            val prov = d.optBoolean("provisioned")
            val sb = StringBuilder(prefix)
            if (!prov) {
                sb.append("Display needs Nightscout — tap Set up.")
            } else if (d.optString("last_result") == "ok" && d.has("last_sgv")) {
                sb.append("Online — ${d.optInt("last_sgv")} mg/dL")
                if (d.has("last_age_min")) sb.append(", ${d.optInt("last_age_min")} min ago")
            } else {
                sb.append("Online — ${d.optString("last_result")}")
            }
            if (d.optBoolean("connected")) sb.append("\nWi-Fi: ${d.optString("ssid")}")
            tv.text = sb.toString()
        } catch (e: Exception) {
            tv.text = "Unexpected reply: $body"
        }
    }
}
