#include <Arduino.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include "esp_camera.h"

#include "img_converters.h"
#include "esp_heap_caps.h"

#include <Preferences.h>

// Timing and threshold
unsigned long CAPTURE_INTERVAL = 1000;  // ms between captures
uint32_t MOTION_THRESHOLD = 3;          // motion sensitivity threshold
int PIXEL_CHECK = 1;                    // how many pixels check per line in a block (1 - minimum; bigger = faster)
int BLOCKS_X = 12;                      // number of width check cells
int BLOCKS_Y = 8;                       // number of height check cells
// Discord
String DISCORD_WEBHOOK = "";            // Discord webhook URL


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
  config.fb_count     = 2;

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

  // Custom parameters for configuration portal
  WiFiManagerParameter webhookParam;
  WiFiManagerParameter intervalParam;
  WiFiManagerParameter thresholdParam;
  WiFiManagerParameter pixelStepParam;
  WiFiManagerParameter blocksXParam;
  WiFiManagerParameter blocksYParam;

  WiFiModule()
  : webhookParam("webhook", "Discord Webhook URL", "", 256),
    intervalParam("interval", "Capture Interval (ms)", "1000", 10),
    thresholdParam("threshold", "Motion Threshold", "3", 5),
    pixelStepParam("pixelstep", "Pixel Step (1=precise, >1=faster)", "1", 5),
    blocksXParam("blocksX", "Blocks X (more = precise)", "12", 5),
    blocksYParam("blocksY", "Blocks Y (more = precise)", "8", 5)
  {}

  void init()
  {
    // ---- Load saved parameters from NVS ----
    prefs.begin("config", true);
    String savedWebhook  = prefs.getString("webhook", "");
    int savedInterval    = prefs.getInt("interval", 1000);
    int savedThreshold   = prefs.getInt("threshold", 3);
    int savedPixelStep   = prefs.getInt("pixelstep", 1);
    int savedBlocksX     = prefs.getInt("blocksX", 12);
    int savedBlocksY     = prefs.getInt("blocksY", 8);
    prefs.end();

    // Fill the portal fields with stored values
    if (savedWebhook.length() > 0)
      webhookParam.setValue(savedWebhook.c_str(), savedWebhook.length());

    String sInterval = String(savedInterval);
    intervalParam.setValue(sInterval.c_str(), sInterval.length());

    String sThreshold = String(savedThreshold);
    thresholdParam.setValue(sThreshold.c_str(), sThreshold.length());

    String sPixel = String(savedPixelStep);
    pixelStepParam.setValue(sPixel.c_str(), sPixel.length());

    String sBlocksX = String(savedBlocksX);
    blocksXParam.setValue(sBlocksX.c_str(), sBlocksX.length());

    String sBlocksY = String(savedBlocksY);
    blocksYParam.setValue(sBlocksY.c_str(), sBlocksY.length());

    // Add parameters to WiFiManager portal
    wm.addParameter(&webhookParam);
    wm.addParameter(&intervalParam);
    wm.addParameter(&thresholdParam);
    wm.addParameter(&pixelStepParam);
    wm.addParameter(&blocksXParam);
    wm.addParameter(&blocksYParam);

    // ---- Try Wi-Fi connection or start AP portal ----
    if (!wm.autoConnect("ESP32-Setup", "12345678"))
    {
      Serial.println("Failed to connect, running AP mode");
      return;
    }

    // ---- Save new parameters into NVS ----
    prefs.begin("config", false);
    prefs.putString("webhook", webhookParam.getValue());
    prefs.putInt("interval", String(intervalParam.getValue()).toInt());
    prefs.putInt("threshold", String(thresholdParam.getValue()).toInt());
    prefs.putInt("pixelstep", String(pixelStepParam.getValue()).toInt());
    prefs.putInt("blocksX", String(blocksXParam.getValue()).toInt());
    prefs.putInt("blocksY", String(blocksYParam.getValue()).toInt());
    prefs.end();

    // ---- Update global variables ----
    DISCORD_WEBHOOK  = String(webhookParam.getValue());
    CAPTURE_INTERVAL = String(intervalParam.getValue()).toInt();
    MOTION_THRESHOLD = String(thresholdParam.getValue()).toInt();
    PIXEL_CHECK      = String(pixelStepParam.getValue()).toInt();
    BLOCKS_X         = String(blocksXParam.getValue()).toInt();
    BLOCKS_Y         = String(blocksYParam.getValue()).toInt();

    // ---- Sanity checks ----
    if (CAPTURE_INTERVAL < 100) CAPTURE_INTERVAL = 100;   // prevent zero or too small interval
    if (MOTION_THRESHOLD < 1)   MOTION_THRESHOLD = 1;     // prevent zero threshold
    if (PIXEL_CHECK < 1)        PIXEL_CHECK = 1;          // prevent below 1
    if (BLOCKS_X < 1)           BLOCKS_X = 1;             // prevent below 1
    if (BLOCKS_Y < 1)           BLOCKS_Y = 1;             // prevent below 1

    // ---- Debug info ----
    Serial.println("Connected to Wi-Fi!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Webhook: ");
    Serial.println(DISCORD_WEBHOOK);
    Serial.print("Interval: ");
    Serial.println(CAPTURE_INTERVAL);
    Serial.print("Threshold: ");
    Serial.println(MOTION_THRESHOLD);
    Serial.print("Pixel Step: ");
    Serial.println(PIXEL_CHECK);
    Serial.print("Blocks X: ");
    Serial.println(BLOCKS_X);
    Serial.print("Blocks Y: ");
    Serial.println(BLOCKS_Y);
  }

  void update()
  {
    // Keeps WiFiManager internal processes running
    wm.process();
  }
};

// ================== Discord Module ==================
class DiscordModule
{
public:
  String webhook;
  bool messageSent = false;

  void init()
  {
    webhook = DISCORD_WEBHOOK;
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
    if (WiFi.status() != WL_CONNECTED || webhook.length() == 0)
    return;
  
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
    delay(10);
  }

  void sendImage(uint8_t *buf, size_t len, const String &filenameSuffix = "")
  {
    // Exit if not connected or webhook is not set
    if (WiFi.status() != WL_CONNECTED || webhook.length() == 0) return;

    HTTPClient http;
    String boundary = "----esp32formboundary";

    // Generate filename with optional suffix
    String filename = "photo_" + filenameSuffix + ".jpg";

    // Multipart head
    String head = "--" + boundary + "\r\n";
    head += "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n";
    head += "Content-Type: image/jpeg\r\n\r\n";

    // Multipart tail (closing boundary)
    String tail = "\r\n--" + boundary + "--\r\n";

    // Calculate total length (head + image + tail)
    size_t totalLen = head.length() + len + tail.length();

    // Allocate buffer for the whole body
    uint8_t *body = (uint8_t*)heap_caps_malloc(totalLen, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!body)
    {
      Serial.println("malloc failed");
      return;
    }

    // Copy parts into the buffer
    memcpy(body, head.c_str(), head.length());
    memcpy(body + head.length(), buf, len);
    memcpy(body + head.length() + len, tail.c_str(), tail.length());
    // Perform HTTP POST with full body
    http.begin(webhook);
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    int code = http.POST(body, totalLen);
    // Free buffer
    free(body);

    // Debug result
    if (code > 0) {
      Serial.printf("Image upload response code: %d\n", code);
    } else {
      Serial.print("Upload failed: ");
      Serial.println(http.errorToString(code));
    }

    http.end();
    delay(10);
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
  // rgb buffer
  static uint8_t* rgb;
  static size_t rgbSize;

  std::vector<uint8_t> compress(const camera_fb_t *fb)
  {
    int pixelStep = PIXEL_CHECK; // increase for speed
    size_t out_len = fb->width * fb->height * 3;

    // allocate once or resize if needed
    if (!rgb || out_len > rgbSize)
    {
      if (rgb) free(rgb);
      rgb = (uint8_t*)heap_caps_malloc(out_len, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
      rgbSize = out_len;
    }
    if (!rgb) return {};

    // JPEG -> RGB888 into static buffer
    if (!fmt2rgb888(fb->buf, fb->len, fb->format, rgb)) {
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
        blocks[by * blocksX + bx] = pxCount ? (sum / pxCount) : 0;
      }

    return blocks;
  }

public:
  MotionDetector() : blocksX(12), blocksY(8) {}

  void init(const camera_fb_t *fb)
  {
    reference = compress(fb);
    hasReference = !reference.empty();

    blocksX = BLOCKS_X;
    blocksY = BLOCKS_Y;
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

uint8_t* MotionDetector::rgb = nullptr;
size_t MotionDetector::rgbSize = 0;

void setup()
{
  pinMode(0, INPUT_PULLUP); // on reset button allow to change settings

  wifi.init();
  discord.init();

  initConfig();
  camera.init(config);

  // ---- Wait for Wi-Fi with timeout ----
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 20000) // 20s timeout
  {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("\nWi-Fi connect failed, starting config portal...");
    wifi.wm.startConfigPortal("ESP32-Setup", "12345678");
    ESP.restart(); // reboot after config
    return; // safety guard
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
    discord.sendImage(fb->buf, fb->len, String(motionCount));
  }
  // release frame buffer always
  camera.release(fb);
}
