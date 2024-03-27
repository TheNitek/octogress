#include <lvgl.h>

#include <bb_spi_lcd.h>
#include <display/lv_bb_spi_lcd.h>

#include <bb_captouch.h>

#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClient.h>

#include "FS.h"
#include <LittleFS.h>

#include <ArduinoJson.h>
#include <OctoPrintAPI.h>

BBCapTouch bbct;
TOUCHINFO ti;

uint16_t touchMinX = TOUCH_MIN_X, touchMaxX = TOUCH_MAX_X, touchMinY = TOUCH_MIN_Y, touchMaxY = TOUCH_MAX_Y;

const uint32_t api_mtbs = 5000;
uint32_t api_lasttime = 0;

char octo_server[40] = "192.168.1.154";
uint16_t octo_port = 80;
char octo_token[33] = "YOUR_API_TOKEN";

WiFiClient client;
OctoprintApi api;

lv_obj_t* arc;
lv_obj_t* fileLabel;
lv_obj_t* stateLabel;
lv_obj_t* tempLabel;
lv_obj_t* remainLabel;

bool shouldSaveConfig = false;

void saveConfigCallback() {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void touch_read( lv_indev_t * indev, lv_indev_data_t * data ) {

  // Capacitive touch needs to be mapped to display pixels
  if(bbct.getSamples(&ti)) {
    if(ti.x[0] < touchMinX) touchMinX = ti.x[0];
    if(ti.x[0] > touchMaxX) touchMaxX = ti.x[0];
    if(ti.y[0] < touchMinY) touchMinY = ti.y[0];
    if(ti.y[0] > touchMaxY) touchMaxY = ti.y[0];

    //Map this to the pixel position
    data->point.x = map(ti.x[0], touchMinX, touchMaxX, 1, lv_display_get_horizontal_resolution(NULL)); // X touch mapping
    data->point.y = map(ti.y[0], touchMinY, touchMaxY, 1, lv_display_get_vertical_resolution(NULL)); // Y touch mapping
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void updateLabel(lv_obj_t* label, const char* newValue) {
  const char* currentText = lv_label_get_text(label);
  // Only update if value has changed
  if(strcmp(currentText, newValue)) {
    lv_label_set_text(label, newValue);
  }
}

static void setAngle(lv_obj_t* arc, int32_t v) {
    lv_arc_set_value(arc, v);
    lv_obj_send_event(arc, LV_EVENT_VALUE_CHANGED, NULL);
}

static void value_changed_event_cb(lv_event_t * e) {
    lv_obj_t * arc = (lv_obj_t*) lv_event_get_target(e);
    lv_obj_t * label = (lv_obj_t*) lv_event_get_user_data(e);

    lv_label_set_text_fmt(label, "%" LV_PRId32 "%%", lv_arc_get_value(arc));
}

void progressBar(void) {
    /*Create an Arc*/
    arc = lv_arc_create(lv_screen_active());
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_obj_set_size(arc, 240, 240);
    lv_arc_set_value(arc, 0);
    lv_obj_set_style_arc_width(arc, 25, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 25, LV_PART_INDICATOR);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);   /*Be sure the knob is not displayed*/
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);  /*To not allow adjusting by click*/
    lv_obj_center(arc);

    lv_obj_t * progressLabel = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(progressLabel, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(progressLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(progressLabel, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_add_event_cb(arc, value_changed_event_cb, LV_EVENT_VALUE_CHANGED, progressLabel);
    lv_obj_send_event(arc, LV_EVENT_VALUE_CHANGED, NULL);

    remainLabel = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_align(remainLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(remainLabel, "");
    lv_obj_align(remainLabel, LV_ALIGN_TOP_MID, 0, 60);

    fileLabel = lv_label_create(lv_screen_active());
    lv_label_set_long_mode(fileLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(fileLabel, 160);
    lv_label_set_text(fileLabel, "");
    lv_obj_align(fileLabel, LV_ALIGN_TOP_MID, 0, 80);

    tempLabel = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_align(tempLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(tempLabel, "");
    lv_obj_align(tempLabel, LV_ALIGN_BOTTOM_MID, 0, -50);

    stateLabel = lv_label_create(lv_screen_active());
    lv_label_set_long_mode(stateLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(stateLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(stateLabel, 100);
    lv_label_set_text(stateLabel, "");
    lv_obj_align(stateLabel, LV_ALIGN_BOTTOM_MID, 0, -20);
}

void showError(const char* msg) {
  lv_obj_t * errorLabel = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(errorLabel, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_align(errorLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(errorLabel, LV_LABEL_LONG_WRAP);
  lv_label_set_text(errorLabel, msg);
  lv_obj_center(errorLabel);
  lv_timer_periodic_handler();
  while(1) yield();
}

void setup() {
  Serial.begin(115200);

  // Initialise LVGL
  lv_init();
  lv_tick_set_cb(millis);
  lv_display_t* disp = lv_bb_spi_lcd_create(DISPLAY_TYPE);

  // Initialize touch screen
  bbct.init(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);

  // Register touch
  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);  
  lv_indev_set_read_cb(indev, touch_read);

  if(!LittleFS.begin(true)){
    showError("LittleFS mount failed");
  }

  if (LittleFS.exists("/config.json")) {
    //file exists, reading and loading
    Serial.println("reading config file");
    File configFile = LittleFS.open("/config.json", "r");
    if (configFile) {
      Serial.println("opened config file");
      size_t size = configFile.size();
      // Allocate a buffer to store contents of the file.
      std::unique_ptr<char[]> buf(new char[size]);

      configFile.readBytes(buf.get(), size);

      JsonDocument json;
      auto deserializeError = deserializeJson(json, buf.get());

      if (deserializeError ) {
        showError("failed to load json config");
      }

      strcpy(octo_server, json["octo_server"]);
      octo_port = json["octo_port"].as<uint16_t>();
      strcpy(octo_token, json["octo_token"]);
      configFile.close();
    }
  }

  const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  char password[8 + 1] = {'\0'};
  for(int p = 0, i = 0; i < 8; i++){
    int r = random(0, strlen(chars));
    password[p++] = chars[r];
  }

  char ssid[] = "octogress-000000";
  for(int p = 10, i = 0; i < 6; i++){
    int r = random(0, strlen(chars));
    ssid[p++] = chars[r];
  }

  WiFiManager wm;
  wm.setConfigPortalBlocking(false);
  wm.setSaveConfigCallback(saveConfigCallback);

  char port[6]; 
  sprintf(port,"%ld", octo_port);

  WiFiManagerParameter custom_octo_server("server", "Octoprint IP", octo_server, 40);
  WiFiManagerParameter custom_octo_port("port", "Octoprint Port", port, 6);
  WiFiManagerParameter custom_octo_token("apikey", "API token", octo_token, 32);

  wm.addParameter(&custom_octo_server);
  wm.addParameter(&custom_octo_port);
  wm.addParameter(&custom_octo_token);


  if(!wm.autoConnect(ssid, password)) {
    lv_obj_t * panel = lv_tileview_create(lv_screen_active());
    lv_obj_set_size(panel, lv_display_get_physical_horizontal_resolution(NULL), lv_display_get_physical_vertical_resolution(NULL));
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t * qrTile = lv_tileview_add_tile(panel, 0, 0, LV_DIR_RIGHT);

    lv_obj_t * slideLabel = lv_label_create(qrTile);
    lv_obj_set_style_text_font(slideLabel, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(slideLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(slideLabel, ">");
    lv_obj_align(slideLabel, LV_ALIGN_RIGHT_MID, -5, 0);


    lv_obj_t * qr = lv_qrcode_create(qrTile);
    lv_qrcode_set_size(qr, 165);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());

    char qrData[100];
    sprintf(qrData, "WIFI:S:%s;T:WPA;P:%s;H:false;", ssid, password);
    lv_qrcode_update(qr, qrData, strlen(qrData));
    lv_obj_center(qr);

    lv_obj_t * labelTile = lv_tileview_add_tile(panel, 1, 0, LV_DIR_LEFT);

    lv_obj_t * wifiLabel = lv_label_create(labelTile);
    lv_label_set_text_fmt(wifiLabel, "AP: %s\nPassword: %s", ssid, password);
    lv_obj_set_style_text_font(wifiLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(wifiLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(wifiLabel);

    while(WiFi.status() != WL_CONNECTED){
      wm.process();
      lv_timer_periodic_handler();
    }

    strcpy(octo_server, custom_octo_server.getValue());
    if(sscanf(custom_octo_port.getValue(), "%d", &octo_port) < 0) {
      showError("Invalid Port Number");
    }
    strcpy(octo_token, custom_octo_token.getValue());

    if (shouldSaveConfig) {
      Serial.println("saving config");
      JsonDocument json;
      json["octo_server"] = octo_server;
      json["octo_port"] = octo_port;
      json["octo_token"] = octo_token;

      File configFile = LittleFS.open("/config.json", "w");
      if (!configFile) {
        showError("failed to open config file for writing");
      }

      serializeJson(json, Serial);
      serializeJson(json, configFile);
      Serial.println();
      configFile.close();
    }

    lv_obj_clean(lv_screen_active());    
  }

  IPAddress octo_ip;
  if(!octo_ip.fromString(octo_server)) {
    showError("Invalid IP");
  }

  api.init(client, octo_ip, octo_port, octo_token);

  progressBar();

  Serial.println("Setup done");
}

void loop() {   
  lv_timer_periodic_handler();

  if (millis() - api_lasttime > api_mtbs || api_lasttime==0) {
    if(api.getPrinterStatistics()){
      if(!api.printerStats.printerStatePrinting) {
        if(!lv_obj_has_flag(remainLabel, LV_OBJ_FLAG_HIDDEN)) {
          lv_obj_add_flag(remainLabel, LV_OBJ_FLAG_HIDDEN);
        }
      } else {
        if(lv_obj_has_flag(remainLabel, LV_OBJ_FLAG_HIDDEN)) {
          lv_obj_clear_flag(remainLabel, LV_OBJ_FLAG_HIDDEN);
        }
      }

      updateLabel(stateLabel, api.printerStats.printerState.c_str());

      char temps[20];
      sprintf(temps, "%.1f°C   %.1f°C", api.printerStats.printerTool0TempActual, api.printerStats.printerBedTempActual);
      updateLabel(tempLabel, temps);
    }
    if(api.getPrintJob()) {
      setAngle(arc, api.printJob.progressCompletion);
      updateLabel(fileLabel, api.printJob.jobFileName.c_str());

      char remainString[30];
      uint32_t rSeconds = api.printJob.progressPrintTimeLeft;
      if(rSeconds < 60) {
        sprintf(remainString, "%d seconds left", rSeconds);
      } else if (rSeconds < 60*60) {
        if(rSeconds % 60) {
          sprintf(remainString, "%.1f minutes left", rSeconds/60.0f);
        } else {
          sprintf(remainString, "%d minutes left", rSeconds/60);
        }
      } else if (rSeconds < 24*60*60) {
        sprintf(remainString, "%.1f hours left", rSeconds/3600.0f);
      } else {
        sprintf(remainString, "%.1f days left", rSeconds/86400.0f);
      }
      updateLabel(remainLabel, remainString);
    }
    api_lasttime = millis();
  }
}