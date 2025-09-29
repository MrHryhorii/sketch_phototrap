#include <Arduino.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include "esp_camera.h"

// Send settings
const char* WEBHOOK_URL = "https://discord.com/api/webhooks/1417920471592079360/q2cZ47f8lfYEjiucKH9maS7IBV57DyHPLhPrnY0CQJxxK_ZIqmne98x20Lg9zGgA41tY";

// Timing and threshold
const unsigned long CAPTURE_INTERVAL = 1000; // extra pause ms between captures

// Threshold for motion detection: 
// smaller value = more sensitive (reacts to small changes)
// larger value  = less sensitive (reacts only to big changes)
const uint32_t MOTION_THRESHOLD = 10;

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

    http.begin(WEBHOOK_URL);
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

    http.begin(WEBHOOK_URL);
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
  uint32_t referenceValue = 0;

  // compress image buffer to a single average value
  uint32_t compress(const uint8_t *buf, size_t len)
  {
    uint32_t sum = 0;
    size_t count = 0;

    // sample every 32nd byte for speed
    for (size_t i = 0; i < len; i += 32)
    {
      sum += buf[i];
      count++;
    }

    return (count > 0) ? (sum / count) : 0;
  }

public:
  // initialize baseline
  void init(const uint8_t *buf, size_t len)
  {
    referenceValue = compress(buf, len);
    hasReference = true;
  }

  // compare new frame to baseline
  bool compare(const uint8_t *buf, size_t len, uint32_t threshold)
  {
    uint32_t current = compress(buf, len);

    if (!hasReference)
    {
      referenceValue = current;
      hasReference = true;
      return false; // nothing to compare yet
    }

    uint32_t diff = (current > referenceValue) ? 
                    (current - referenceValue) : 
                    (referenceValue - current);

    // always update baseline then gradual changes won’t trigger
    referenceValue = current;

    return diff > threshold;
  }
};


// ================== Main Program ==================
WiFiModule wifi;
DiscordModule discord;
CameraModule camera;
// Timing
unsigned long lastCapture = 0;
// Motion detector
MotionDetector detector;

void setup()
{
  wifi.init();
  discord.init();

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
    detector.init(fb->buf, fb->len);

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

  // --- Timing for capture ---
  if (millis() - lastCapture < CAPTURE_INTERVAL) return;
  lastCapture = millis();
  // --- Capture frame ---
  camera_fb_t *fb = camera.capture();
  if (!fb) return;
  // --- Motion detection ---
  bool motion = detector.compare(fb->buf, fb->len, MOTION_THRESHOLD);
  if (motion)
  {
    Serial.println("Motion detected!");
    discord.sendText("Motion detected!");
    discord.sendImage(fb->buf, fb->len);
  }
  // release frame buffer always
  camera.release(fb);
}
