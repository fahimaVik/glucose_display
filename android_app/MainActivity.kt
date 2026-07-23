package com.example.glucosewifi

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONArray
import org.json.JSONObject
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

// A small controller for the glucose display's WiFi. It talks to the device's
// HTTP API - the same one you can hit in a browser at http://192.168.4.1/api/
// - to list, add and remove the networks the device remembers.
//
// All networking runs on a background thread; results are posted back to the
// UI thread. No third-party libraries are used, only the Android SDK, so the
// project builds with a stock "Empty Views Activity" template.
class MainActivity : AppCompatActivity() {

    private val main = Handler(Looper.getMainLooper())

    private lateinit var addr: EditText
    private lateinit var status: TextView
    private lateinit var savedList: LinearLayout
    private lateinit var ssid: EditText
    private lateinit var pass: EditText
    private lateinit var log: TextView

    private fun base() = "http://" + addr.text.toString().trim()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        addr = findViewById(R.id.addr)
        status = findViewById(R.id.status)
        savedList = findViewById(R.id.savedList)
        ssid = findViewById(R.id.ssid)
        pass = findViewById(R.id.pass)
        log = findViewById(R.id.log)

        findViewById<Button>(R.id.refresh).setOnClickListener { refresh() }
        findViewById<Button>(R.id.save).setOnClickListener { addNetwork() }

        findViewById<CheckBox>(R.id.showPass).setOnCheckedChangeListener { _, checked ->
            pass.inputType = if (checked)
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
            else
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        }

        refresh()
    }

    // --- networking ---------------------------------------------------------

    private fun http(method: String, path: String, body: String?, done: (Int, String) -> Unit) {
        Thread {
            var code = -1
            var resp = ""
            try {
                val c = URL(base() + path).openConnection() as HttpURLConnection
                c.requestMethod = method
                c.connectTimeout = 5000
                c.readTimeout = 8000
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

    private fun setLog(s: String) { log.text = s }

    // --- actions ------------------------------------------------------------

    private fun refresh() {
        setLog("Contacting device…")
        http("GET", "/api/networks", null) { code, resp ->
            if (code != 200) {
                status.text = "Not reachable"
                setLog("Could not reach the device.\n\n" +
                        "• Are you joined to the GlucoseSetup hotspot (or the same WiFi as the device)?\n" +
                        "• Try turning mobile data OFF so the phone uses the hotspot.\n\n" +
                        "($resp)")
                return@http
            }
            try {
                val o = JSONObject(resp)
                status.text = if (o.optBoolean("connected"))
                    "Online — ${o.optString("current")}  (${o.optString("ip")})"
                else
                    "In setup mode (no network joined yet)"
                renderSaved(o.optJSONArray("saved") ?: JSONArray())
                setLog("Updated.")
            } catch (e: Exception) {
                setLog("Unexpected reply: $resp")
            }
        }
    }

    private fun renderSaved(arr: JSONArray) {
        savedList.removeAllViews()

        if (arr.length() == 0) {
            val t = TextView(this)
            t.text = "(none saved yet)"
            savedList.addView(t)
            return
        }

        for (i in 0 until arr.length()) {
            val name = arr.getString(i)

            val row = LinearLayout(this)
            row.orientation = LinearLayout.HORIZONTAL

            val label = TextView(this)
            label.text = name
            label.textSize = 16f
            label.layoutParams = LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f
            )

            val del = Button(this)
            del.text = "Delete"
            del.setOnClickListener { deleteNetwork(name) }

            row.addView(label)
            row.addView(del)
            savedList.addView(row)
        }
    }

    private fun addNetwork() {
        val s = ssid.text.toString().trim()
        val p = pass.text.toString()

        if (s.isEmpty()) { setLog("Enter a network name first."); return }

        val body = JSONObject().put("ssid", s).put("pass", p).toString()
        setLog("Saving “$s”…")

        http("POST", "/api/networks", body) { code, resp ->
            if (code == 200) {
                setLog("Saved. The device will try to join “$s” now.")
                ssid.setText("")
                pass.setText("")
                // Give the device a few seconds to switch networks, then re-read.
                main.postDelayed({ refresh() }, 6000)
            } else {
                setLog("Save failed: $resp")
            }
        }
    }

    private fun deleteNetwork(name: String) {
        setLog("Removing “$name”…")
        val q = "/api/networks?ssid=" + URLEncoder.encode(name, "UTF-8")
        http("DELETE", q, null) { code, resp ->
            if (code == 200) { setLog("Removed “$name”."); refresh() }
            else setLog("Remove failed: $resp")
        }
    }
}
