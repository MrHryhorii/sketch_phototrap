#include <Arduino.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include "esp_camera.h"

#include "img_converters.h"
#include "esp_heap_caps.h"

#include <Preferences.h>

// Timing and threshold
const unsigned long CAPTURE_INTERVAL = 1000; // extra pause ms between captures

// Threshold for motion detection: 
// smaller value = more sensitive (reacts to small changes)
// larger value  = less sensitive (reacts only to big changes)
const uint32_t MOTION_THRESHOLD = 3;

// ================== Camera Config Profile ==================
camera_config_t config;

void initConfig()
{
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_pwdn     = -1;
  config.pin_reset    = -1;
  config.pin_xclk     = 15;
  config.pin_sccb_sda = 4;
  config.pin_sccb_scl = 5;
  config.pin_d0       = 11;
  config.pin_d1       = 9;
  config.pin_d2       = 8;
  config.pin_d3       = 10;
  config.pin_d4       = 12;
  config.pin_d5       = 18;
  config.pin_d6       = 17;
  config.pin_d7       = 16;
  config.pin_vsync    = 6;
  config.pin_href     = 7;
  config.pin_pclk     = 13;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.fb_count     = 1;

  if (psramFound())
  {
    config.frame_size  = FRAMESIZE_VGA;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 10;
  } else {
    config.frame_size  = FRAMESIZE_QVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.jpeg_quality = 15;
  }
}


// ================== WiFi Module ==================
class WiFiModule
{
public:
  WiFiManager wm;
  Preferences prefs;

  // Custom parameters
  WiFiManagerParameter webhookParam;

  WiFiModule()
  : webhookParam("webhook", "Discord Webhook URL", "", 256) // id, placeholder, default, length
  {}

  void init()
  {
    Serial.begin(115200);

    // load saved webhook
    prefs.begin("config", true);
    String saved = prefs.getString("webhook", "");
    prefs.end();
    if (saved.length() > 0)
    {
      webhookParam.setValue(saved.c_str(), saved.length());
    }

    // Attach custom parameter
    wm.addParameter(&webhookParam);

    // Try connect
    if (!wm.autoConnect("ESP32-Setup", "12345678"))
    {
      Serial.println("Failed to connect, running AP mode");
      return;
    }

    // Save params
    prefs.begin("config", false);
    prefs.putString("webhook", webhookParam.getValue());
    prefs.end();

    // check if webhook is set
    String webhookNow = getWebhook();
    if (webhookNow.length() == 0)
    {
      Serial.println("Webhook not set! Staying in AP mode...");
      delay(1000);
      wm.startConfigPortal("ESP32-Setup", "12345678"); // force AP until user fills in
    } else {
      Serial.println("Connected to Wi-Fi!");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
    }
  }
  // Get Preferences
  String getWebhook()
  {
    prefs.begin("config", true);
    String saved = prefs.getString("webhook", "");
    prefs.end();
    return saved;
  }

  void update()
  {
    wm.process();
  }
};

// ================== Discord Module ==================
class DiscordModule
{
public:
  String webhook;
  bool messageSent = false;

  void init(const String &webhookUrl)
  {
    webhook = webhookUrl;
  }

  void update()
  {
    if (WiFi.status() == WL_CONNECTED && !messageSent)
    {
      sendText("ESP32 is online and connected!");
      messageSent = true;
    }
  }

  void sendText(const String &content)
  {
    HTTPClient http;

    http.begin(webhook);
    http.addHeader("Content-Type", "application/json");

    String payload = "{\"content\":\"" + content + "\"}";
    int httpResponseCode = http.POST(payload);

    if (httpResponseCode > 0)
    {
      Serial.print("Message sent, code: ");
      Serial.println(httpResponseCode);
    }
    else
    {
      Serial.print("Error: ");
      Serial.println(http.errorToString(httpResponseCode));
    }

    http.end();
  }

  void sendImage(uint8_t *buf, size_t len)
  {
    if (WiFi.status() != WL_CONNECTED)
      return;

    HTTPClient http;
    String boundary = "----esp32formboundary"; // custom boundary for multipart request

    http.begin(webhook);
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

    // ---- Prepare multipart body ----
    // "head" contains headers for the file upload
    String head = "--" + boundary + "\r\n";
    head += "Content-Disposition: form-data; name=\"file\"; filename=\"photo.jpg\"\r\n";
    head += "Content-Type: image/jpeg\r\n\r\n";

    // "tail" closes the multipart message
    String tail = "\r\n--" + boundary + "--\r\n";

    // Calculate total body length
    int totalLen = head.length() + len + tail.length();

    // Allocate buffer to hold the whole request body
    uint8_t *body = (uint8_t *)malloc(totalLen);
    if (!body)
    {
      Serial.println("Body malloc failed");
      return;
    }

    // Copy parts into the buffer: head + image + tail
    memcpy(body, head.c_str(), head.length());
    memcpy(body + head.length(), buf, len);
    memcpy(body + head.length() + len, tail.c_str(), tail.length());

    // ---- Send POST request with binary data ----
    int code = http.POST(body, totalLen);

    // Free the allocated memory after sending
    free(body);

    // ---- Print the result ----
    if (code > 0)
    {
      Serial.printf("Image upload response code: %d\n", code);
    }
    else
    {
      Serial.print("Upload failed, error: ");
      Serial.println(http.errorToString(code));
    }

    http.end();
  }
};

// ================== Camera Module ==================
class CameraModule
{
public:
  bool initialized = false;

  bool init(camera_config_t &config)
  {
    Serial.println("Calling esp_camera_init...");
    esp_err_t err = esp_camera_init(&config);
    if (err == ESP_OK)
    {
      Serial.println("Camera init OK");
      initialized = true;
    }
    else
    {
      Serial.printf("Camera init FAIL, error 0x%x\n", err);
      initialized = false;
    }
    return initialized;
  }

  camera_fb_t *capture()
  {
    if (!initialized)
      return nullptr;
    return esp_camera_fb_get();
  }

  void release(camera_fb_t *fb)
  {
    if (fb)
      esp_camera_fb_return(fb);
  }
};

// ================== Motion Detector Module ==================
class MotionDetector
{
private:
  bool hasReference = false;
  std::vector<uint8_t> reference;
  int blocksX, blocksY;

  std::vector<uint8_t> compress(const camera_fb_t *fb)
  {
    int pixelStep = 1;
    // JPEG to RGB888
    size_t out_len = fb->width * fb->height * 3;
    uint8_t *rgb = (uint8_t*)heap_caps_malloc(out_len, MALLOC_CAP_8BIT);
    if (!rgb) return {};
    if (!fmt2rgb888(fb->buf, fb->len, fb->format, rgb))
    {
      free(rgb);
      return {};
    }
    // matrix of average brightness per block
    std::vector<uint8_t> blocks(blocksX * blocksY, 0);
    // block size in pixels
    int bw = fb->width  / blocksX;
    int bh = fb->height / blocksY;

    // go through all blocks vertically
    for (int by = 0; by < blocksY; by++)
      // go through all blocks horizontally
      for (int bx = 0; bx < blocksX; bx++)
      {
        // sum of grayscale values
        uint32_t sum = 0;
        // number of pixels in this block
        int pxCount = 0;
        // loop pixels inside block (rows)
        for (int y = 0; y < bh; y += pixelStep)
        {
          // row start in full image
          int yy = (by * bh + y) * fb->width;
          // loop pixels inside block (cols)
          for (int x = 0; x < bw; x += pixelStep)
          {
            // pixel index (RGB888 to 3 bytes)
            int idx = (yy + bx * bw + x) * 3;
            uint8_t r = rgb[idx], g = rgb[idx+1], b = rgb[idx+2];
            sum += (r * 30 + g * 59 + b * 11) / 100; // grayscale
            pxCount++;
          }
        }
        // save avg brightness of this block
        blocks[by * blocksX + bx] = sum / pxCount;
      }

    free(rgb);
    return blocks;
  }

public:
  MotionDetector(int bx = 12, int by = 8) : blocksX(bx), blocksY(by) {}

  void init(const camera_fb_t *fb)
  {
    reference = compress(fb);
    hasReference = !reference.empty();
  }

  bool compare(const camera_fb_t *fb, uint32_t threshold)
  {
    auto current = compress(fb);
    if (current.empty()) return false;

    if (!hasReference)
    {
      reference = current;
      hasReference = true;
      return false;
    }

    // is there motion?
    bool triggered = false;
    for (size_t i = 0; i < current.size(); i++) {
        int diff = abs((int)current[i] - (int)reference[i]);
        if (diff > (int)threshold) {
            triggered = true; // movement found in this block
            break;
        }
    }

    reference = current; // update for next frame
    return triggered;    // result of check
  }
};


// ================== Main Program ==================
WiFiModule wifi;
DiscordModule discord;
CameraModule camera;
// Timing
unsigned long lastCapture = 0;
int motionCount = 0; // global counter
// Motion detector
MotionDetector detector;

void setup()
{
  pinMode(0, INPUT_PULLUP); // on reset button allow to change settings

  wifi.init();
  discord.init(wifi.getWebhook());

  initConfig();
  camera.init(config);

  // Wi-Fi stabilize
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(500);
    Serial.print(".");
  } 

  if (!camera.initialized)
  {
    Serial.println("Camera not initialized, skipping capture");
    return;
  }

  camera_fb_t *fb = camera.capture();
  if (fb)
  {
    String msg = "Photo captured! Size: " + String(fb->len) + " bytes (" +
                String(fb->width) + "x" + String(fb->height) + ")";
    Serial.println(msg);
    discord.sendText(msg);
    discord.sendImage(fb->buf, fb->len);

    // init Motion Detector
    detector.init(fb);

    camera.release(fb);
  }
  else
  {
    discord.sendText("Failed to capture image!");
  }
}

void loop()
{
  // --- Update modules ---
  wifi.update();
  discord.update();

  // --- Set Up mode ---
  if (digitalRead(0) == LOW)
  {
    Serial.println("Button pressed!");
    wifi.wm.startConfigPortal("ESP32-Setup", "12345678");
    ESP.restart();
  }

  // --- Timing for capture ---
  if (millis() - lastCapture < CAPTURE_INTERVAL) return;
  lastCapture = millis();
  // --- Capture frame ---
  camera_fb_t *fb = camera.capture();
  if (!fb) return;
  // --- Motion detection ---
  bool motion = detector.compare(fb, MOTION_THRESHOLD);
  if (motion)
  {
    motionCount++;
    String msg = "Motion detected! #" + String(motionCount);
    Serial.println(msg);
    discord.sendText(msg);
    discord.sendImage(fb->buf, fb->len);
  }
  // release frame buffer always
  camera.release(fb);
}
