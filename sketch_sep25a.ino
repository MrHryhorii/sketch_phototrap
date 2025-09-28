#include <Arduino.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include "esp_camera.h"

// Send settings
const char* WEBHOOK_URL = "https://discord.com/api/webhooks/1417920471592079360/q2cZ47f8lfYEjiucKH9maS7IBV57DyHPLhPrnY0CQJxxK_ZIqmne98x20Lg9zGgA41tY";

// Low-quality (fast) settings
const framesize_t LOW_FRAMESIZE = FRAMESIZE_QVGA; // or FRAMESIZE_QQVGA
const int LOW_JPEG_QUALITY = 30; // bigger = lower quality (arduino-esp: 0..63, lower = better)
const camera_fb_location_t LOW_FB_LOCATION = CAMERA_FB_IN_DRAM; // prefer DRAM for speed

// High-quality (send) settings
const framesize_t HIGH_FRAMESIZE = FRAMESIZE_VGA;
const int HIGH_JPEG_QUALITY = 4;
const camera_fb_location_t HIGH_FB_LOCATION = CAMERA_FB_IN_PSRAM; // PSRAM for large frame

// Timing and threshold
const unsigned long LOW_INTERVAL_MS = 1000; // time between cheap captures
const uint32_t MOTION_THRESHOLD = 15; // percent-ish or MAD-based threshold (tune)

// ================== Camera Config Profiles ==================
camera_config_t lowConfig;
camera_config_t highConfig;

void initConfigs() {
  // Low config
  lowConfig.ledc_channel = LEDC_CHANNEL_0;
  lowConfig.ledc_timer   = LEDC_TIMER_0;
  lowConfig.pin_pwdn     = -1;
  lowConfig.pin_reset    = -1;
  lowConfig.pin_xclk     = 15;
  lowConfig.pin_sccb_sda = 4;
  lowConfig.pin_sccb_scl = 5;
  lowConfig.pin_d0       = 11;
  lowConfig.pin_d1       = 9;
  lowConfig.pin_d2       = 8;
  lowConfig.pin_d3       = 10;
  lowConfig.pin_d4       = 12;
  lowConfig.pin_d5       = 18;
  lowConfig.pin_d6       = 17;
  lowConfig.pin_d7       = 16;
  lowConfig.pin_vsync    = 6;
  lowConfig.pin_href     = 7;
  lowConfig.pin_pclk     = 13;
  lowConfig.xclk_freq_hz = 20000000;
  lowConfig.pixel_format = PIXFORMAT_JPEG;
  lowConfig.frame_size   = LOW_FRAMESIZE;
  lowConfig.fb_location  = LOW_FB_LOCATION;
  lowConfig.jpeg_quality = LOW_JPEG_QUALITY;
  lowConfig.fb_count     = 1;

  // High config
  highConfig = lowConfig; // copy from lowConfig to highConfig
  highConfig.frame_size   = HIGH_FRAMESIZE;
  highConfig.fb_location  = HIGH_FB_LOCATION;
  highConfig.jpeg_quality = HIGH_JPEG_QUALITY;
}


// ================== WiFi Module ==================
class WiFiModule
{
public:
  WiFiManager wm;

  void init()
  {
    Serial.begin(115200);
    if (!wm.autoConnect("ESP32-Setup", "12345678"))
    {
      Serial.println("Failed to connect, running AP mode");
    }
    else
    {
      Serial.println("Connected to Wi-Fi!");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
    }
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
  bool messageSent = false;

  void init() {}

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
    String webhook_url = WEBHOOK_URL;

    http.begin(webhook_url);
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
    String webhook_url = WEBHOOK_URL;
    String boundary = "----esp32formboundary"; // custom boundary for multipart request

    http.begin(webhook_url);
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

// ================== Main Program ==================
WiFiModule wifi;
DiscordModule discord;
CameraModule camera;

void setup()
{
  wifi.init();
  discord.init();
  initConfigs();

  if (!psramFound()) {
    Serial.println("PSRAM not found, falling back to DRAM");
    highConfig.fb_location = CAMERA_FB_IN_DRAM;
  }
  
  camera.init(highConfig);

  delay(5000); // Wi-Fi stabilize

  if (WiFi.status() == WL_CONNECTED && camera.initialized)
  {
    camera_fb_t *fb = camera.capture();
    if (fb)
    {
      String msg = "Photo captured! Size: " + String(fb->len) + " bytes (" +
                   String(fb->width) + "x" + String(fb->height) + ")";
      Serial.println(msg);
      discord.sendText(msg);
      discord.sendImage(fb->buf, fb->len);
      camera.release(fb);
    }
    else
    {
      discord.sendText("Failed to capture image!");
    }
  }
}

void loop()
{
  wifi.update();
  discord.update();
}
