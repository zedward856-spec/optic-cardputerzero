# Adding Velvet (CardputerZero emulator) to Optic Suite

Velvet is a CardputerZero (M5Stack Pi CM0 handheld) app UI written in C++/LVGL, compiled to
WebAssembly and wrapped in a page that shows the real device: the screen in its frame and a
working keyboard. The whole thing is **one self-contained HTML file** (WASM, images and JS all
inlined, about 6 MB). It runs offline and needs no network.

## The file

- Source of truth: `~/cardputerzero/velvet/velvet-share.html`. Regenerate it with
  `~/cardputerzero/velvet/build-web.sh`, which rebuilds the WASM and bundles the page.
- Copy it into the app as **`app/src/main/assets/velvet/index.html`**. Copy it again whenever
  Velvet changes. Nothing else is needed.

## How to show it

Copy `MinideckActivity.kt`. It already does the right thing: it serves assets under a fake
`https://` origin via `shouldInterceptRequest`. Call the new activity `VelvetActivity` and serve
from `assets/velvet/`.

**Don't load it as `file://` or `content://`.** On the POCO, Chrome gives those pages an opaque
origin and auto-denies the camera and the motion sensors. A real https-looking origin from the
interceptor works.

Load: `https://<HOST>/index.html?app&shell=black`

- `?app` shows only the device, scaled to fill the WebView. The gallery and text around it are
  hidden.
- `shell=black` uses the smoked see-through black shell; `shell=white` uses the white one. With
  no `shell` parameter, the page uses the last choice saved in localStorage.

### WebView settings

```kotlin
settings.javaScriptEnabled = true
settings.domStorageEnabled = true                 // remembers shell colour, theme, sound, tutorial
settings.mediaPlaybackRequiresUserGesture = false // UI sound effects (Web Audio)
settings.allowFileAccess = false
settings.allowContentAccess = false
setBackgroundColor(0xFF060C26.toInt())            // page background colour
```

The page asks for Google Fonts (`fonts.googleapis.com`). Answering 403 is fine: those fonts are
only used by text that `?app` hides.

### Camera (the CAMERA app inside Velvet)

The page calls `getUserMedia({video: {facingMode: 'environment'}})`. The manifest already has
`android.permission.CAMERA`. Add a `WebChromeClient`:

```kotlin
override fun onPermissionRequest(req: PermissionRequest) {
    if (PermissionRequest.RESOURCE_VIDEO_CAPTURE !in req.resources) { req.deny(); return }
    if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
        req.grant(arrayOf(PermissionRequest.RESOURCE_VIDEO_CAPTURE))
    } else {
        pending = req
        requestPermissions(arrayOf(Manifest.permission.CAMERA), 7)
        // in onRequestPermissionsResult: pending?.grant(...) or pending?.deny()
    }
}
```

Only grant this for requests from the fake origin (`req.origin.host == HOST`).

### Motion sensors (the SENSORS app: compass + level)

The motion sensors use `devicemotion` and `deviceorientationabsolute` events. A WebView delivers
these without any permission, so nothing extra is needed. The page rotates the axes using
`screen.orientation.angle`, so any orientation works. Landscape suits the device best:
`android:screenOrientation="sensorLandscape"` on the activity.

### JavaScript bridge: `VelvetAndroid` (optional, recommended)

The page checks for `window.VelvetAndroid`:

| method | when | what to do |
|---|---|---|
| `savePhoto(dataUrl: String): Boolean` | OK pressed in CAMERA | `dataUrl` is `data:image/jpeg;base64,...` at full camera resolution. Decode it and save it with MediaStore to `Pictures/Velvet/velvet-<time>.jpg`. Return true on success; the screen then shows "PHOTO SAVED". |
| `quit()` | ESC held on Velvet's home menu (its "quit") | `runOnUiThread { finish() }` |
| `openUrl(url: String)` | Velvet's browser opens a site the demo can't read (Google etc.) | open it in the phone's browser: `startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))` (only `http`/`https` URLs) |

Without the bridge, `savePhoto` falls back to an `<a download>` of a blob, which a WebView can't
handle. So add the bridge, or the photo is lost. Without `quit`, the page just reloads.

```kotlin
web.addJavascriptInterface(object {
    @JavascriptInterface fun savePhoto(dataUrl: String): Boolean = try {
        val bytes = Base64.decode(dataUrl.substringAfter(','), Base64.DEFAULT)
        val values = ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, "velvet-${System.currentTimeMillis()}.jpg")
            put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
            put(MediaStore.Images.Media.RELATIVE_PATH, "Pictures/Velvet")
        }
        val uri = contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)!!
        contentResolver.openOutputStream(uri)!!.use { it.write(bytes) }
        true
    } catch (e: Exception) { false }
    @JavascriptInterface fun quit() { runOnUiThread { finish() } }
}, "VelvetAndroid")
```

`shouldOverrideUrlLoading` already refuses other hosts, so only the bundled page can reach the
bridge.

### Other notes

- **Back gesture:** `finish()`, like Minideck. The Zero's own ESC is on the drawn keyboard.
- **Optic `Chrome` overlay:** with `?app` the device fills the view, so keep the title bar
  minimal or skip it.
- **Menu:** add `Entry("CardputerZero") { it.startActivity(Intent(it, VelvetActivity::class.java)) }`
  in `MenuActivity.kt`. Optionally add a second entry, or a setting, for `shell=white`.
- **APK size:** grows by about 6 MB, almost all of it the WASM.
- **Input:** everything is driven by taps on the drawn keyboard. No soft keyboard is needed.

## What's inside, for reference

- **Home menu:** TERMINAL, CAMERA, SENSORS, SCHEDULE, NOTES, STATS, WI-FI, SYSTEM. Persona-style
  look, P3 blue / P5 red themes.
- **Keyboard:** the CardputerZero layout. `fn` cycles layers: orange (arrows, F-keys), then blue
  (symbols), then back to normal. A key with no legend on the current layer drops back to normal.
  F/Z/X/C work as arrows in menus.
- **Browser stand-ins:** in the browser the terminal is a demo shell, and Wi-Fi, battery and
  volume are simulated. Sound effects are synthesised (Web Audio).

## Checking it works

1. Install, then open Menu → CardputerZero.
2. The device should fill the screen with the black shell, and the Velvet home menu should be on
   its screen.
3. Tap X twice, then OK, to open SENSORS. The data source should read `PHONE`, and the compass and
   level should move.
4. ESC, then Z, then OK, to open CAMERA. Accept the camera prompt and the viewfinder should show
   the back camera. OK should save to `Pictures/Velvet`.
