// E-reader for Waveshare 3.97" E-Paper with ESP-32
#include <Arduino.h>
#include <SD_MMC.h>
#include <FS.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <string>
#include "EPD_3in97.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include "CoverLoader.h"
#include "Cache.h"
#include "BookReader.h"
#include "WaveshareRenderer.h"
#include "FileList.h"
#include "PowerManager.h"

static const char *TAG = "MAIN";

#define BTN_UP 4
#define BTN_SELECT 5
#define BTN_DOWN 6

#define AXP_SDA 41
#define AXP_SCL 42
#define PWR_IRQ 38

#define SD_CLK 16
#define SD_CMD 17
#define SD_D0 15
#define SD_D1 7
#define SD_D2 8
#define SD_D3 18

#define SLEEP_TIMEOUT_MS (5UL * 60UL * 1000UL)
#define DEBOUNCE_MS 50
#define LONG_PRESS_MS 3000

enum AppState {
  STATE_FILE_LIST,
  STATE_READING
};

static AppState g_state = STATE_FILE_LIST;
static unsigned long g_last_activity = 0;
static bool g_needs_redraw = true;
static bool g_menu_epd4_ready = false;

static UBYTE *g_image_buffer = nullptr;
static UBYTE *g_image_buf4 = nullptr;
static WaveshareRenderer *g_renderer = nullptr;
static FileList *g_file_list = nullptr;
static BookReader *g_book_reader = nullptr;
static PowerManager *g_power = nullptr;

static Preferences g_prefs;

enum LoadJobType { JOB_BOOK, JOB_COVERS, JOB_SLEEP_COVER };

static volatile LoadJobType g_job_type = JOB_BOOK;
static volatile bool g_load_done = false;
static volatile bool g_load_success = false;
static char g_load_path[320] = {0};
static SemaphoreHandle_t g_load_sem = nullptr;

static uint8_t *g_sleep_cover_buf = nullptr;

void background_task(void *param)
{
  while (true)
  {
    if (xSemaphoreTake(g_load_sem, portMAX_DELAY) != pdTRUE) continue;

    if (g_job_type == JOB_BOOK)
    {
      Serial.printf("[LOADER] Loading book: %s\n", g_load_path);
      delete g_book_reader;
      g_book_reader  = new BookReader(g_renderer, g_load_path);
      g_load_success = g_book_reader->load();
      if (!g_load_success)
      {
        Serial.println("[LOADER] Book load failed");
        delete g_book_reader;
        g_book_reader = nullptr;
      }
      else
      {
        Serial.printf("[LOADER] Book OK, sections=%d\n", g_book_reader->total_pages_in_section());
      }
    }
    else if (g_job_type == JOB_COVERS)
    {
      Serial.println("[LOADER] Loading covers...");
      g_file_list->load_covers_on_task_with_wdt();
      Serial.println("[LOADER] Covers done");
      g_load_success = true;
    }
    else
    {
      Serial.println("[LOADER] Loading sleep cover...");
      if (g_sleep_cover_buf) { free(g_sleep_cover_buf); g_sleep_cover_buf = nullptr; }
      g_sleep_cover_buf = cover_load_cached(g_load_path);
      g_load_success = (g_sleep_cover_buf != nullptr);
      Serial.printf("[LOADER] Sleep cover %s\n", g_load_success ? "OK" : "FAILED");
    }

    g_load_done = true;
  }
}

void request_book_load(const char *path)
{
  strncpy(g_load_path, path, sizeof(g_load_path) - 1);
  g_job_type     = JOB_BOOK;
  g_load_done    = false;
  g_load_success = false;
  xSemaphoreGive(g_load_sem);
}

void request_cover_load()
{
  g_job_type     = JOB_COVERS;
  g_load_done    = false;
  g_load_success = false;
  xSemaphoreGive(g_load_sem);
}

void wait_for_loader()
{
  while (!g_load_done) { delay(20); }
}

void init_display();
void init_sd();
void handle_buttons();
void show_cover_and_sleep();
void redraw_screen();
void save_reading_progress();
void request_book_load(const char *path);
void request_cover_load();
void wait_for_loader();

void setup()
{
  Serial.begin(115200);
  delay(500);

  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 30000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&wdt_config);

  ESP_LOGI(TAG, "E-Reader booting...");

  pinMode(BTN_UP,     INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BTN_DOWN,   INPUT_PULLUP);

  Wire.begin(AXP_SDA, AXP_SCL);

  g_power = new PowerManager(Wire, PWR_IRQ);
  g_power->begin();

  init_sd();
  init_display();

  g_renderer = new WaveshareRenderer(g_image_buffer);
  g_load_sem = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(
    background_task,
    "bg_loader",
    32768,
    nullptr,
    1,
    nullptr,
    0
  );

  g_file_list = new FileList();
  g_file_list->scan("/");

  g_prefs.begin("ereader", false);
  String last_book = g_prefs.getString("last_book", "");
  int    last_sect = g_prefs.getInt("last_sect", 0);
  int    last_page = g_prefs.getInt("last_page", 0);

  if (last_book.length() > 0 && g_file_list->find(last_book.c_str()))
  {
    Paint_NewImage(g_image_buffer, EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT, 0, WHITE);
    Paint_SetScale(2);
    Paint_Clear(WHITE);
    Paint_DrawString_EN(280, 220, "Loading...", &Font24, WHITE, BLACK);
    EPD_3IN97_Init_Fast();
    EPD_3IN97_Display_Fast(g_image_buffer);

    request_book_load(last_book.c_str());
    while (!g_load_done) { delay(50); }

    if (g_load_success)
    {
      g_book_reader->go_to(last_sect, last_page);
      g_state = STATE_READING;
    }
  }

  g_last_activity = millis();
  g_needs_redraw  = true;
}

void loop()
{
  handle_buttons();

  if (g_needs_redraw)
  {
    redraw_screen();
    g_needs_redraw = false;
  }

  if (millis() - g_last_activity > SLEEP_TIMEOUT_MS)
  {
    show_cover_and_sleep();
  }

  delay(20);
}

void init_sd()
{
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);

  if (!SD_MMC.begin("/sdcard", false))
  {
    Serial.println("[SD] 4-bit mount failed, trying 1-bit...");
    if (!SD_MMC.begin("/sdcard", true))
    {
      Serial.println("[SD] Mount FAILED in both modes");
      return;
    }
  }

  Serial.println("[SD] Mounted OK");
  Serial.printf("[SD] Card type: %d\n", SD_MMC.cardType());
  Serial.printf("[SD] Card size: %llu MB\n", SD_MMC.cardSize() / (1024 * 1024));
  Serial.println("[SD] Root directory contents:");
  File root = SD_MMC.open("/");
  if (root && root.isDirectory())
  {
    File f = root.openNextFile();
    while (f)
    {
      Serial.printf("[SD]   '%s'  (%s, %lu bytes)\n", f.name(), f.isDirectory() ? "DIR" : "FILE", (unsigned long)f.size());
      f = root.openNextFile();
    }
  }
  else
  {
    Serial.println("[SD] Could not open root /");
  }
}

void init_display()
{
  DEV_Module_Init();
  EPD_3IN97_Init();
  EPD_3IN97_Clear();
  DEV_Delay_ms(500);

  UDOUBLE buf1_size = ((EPD_3IN97_WIDTH % 8 == 0) ? (EPD_3IN97_WIDTH / 8) : (EPD_3IN97_WIDTH / 8 + 1)) * EPD_3IN97_HEIGHT;

  g_image_buffer = (UBYTE *)malloc(buf1_size);
  if (!g_image_buffer)
  {
    Serial.printf("[DISP] Failed to allocate 1-bit buffer (%lu bytes)\n", (unsigned long)buf1_size);
    while (1) { delay(1000); }
  }

  UDOUBLE buf4_size = ((EPD_3IN97_WIDTH % 4 == 0) ? (EPD_3IN97_WIDTH / 4) : (EPD_3IN97_WIDTH / 4 + 1)) * EPD_3IN97_HEIGHT;

  g_image_buf4 = (UBYTE *)heap_caps_malloc(buf4_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_image_buf4) g_image_buf4 = (UBYTE *)malloc(buf4_size);
  if (!g_image_buf4)
  {
    Serial.printf("[DISP] Failed to allocate 4-gray buffer (%lu bytes)\n", (unsigned long)buf4_size);
    while (1) { delay(1000); }
  }

  Paint_NewImage(g_image_buffer, EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT, 0, WHITE);
  Paint_SetScale(2);
  Paint_Clear(WHITE);
  Serial.printf("[DISP] Buffers: 1-bit=%lu B, 4-gray=%lu B\n", (unsigned long)buf1_size, (unsigned long)buf4_size);
}

void redraw_screen()
{
  if (g_state == STATE_FILE_LIST)
  {
    if (g_file_list->needs_cover_load())
    {
      request_cover_load();
      wait_for_loader();
    }

    Paint_NewImage(g_image_buffer, EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT, 0, WHITE);
    Paint_SetScale(2);

    g_file_list->render(g_image_buffer, g_power->battery_percent());

    EPD_3IN97_Init_Fast();
    g_menu_epd4_ready = true;
    EPD_3IN97_Display_Fast(g_image_buffer);
  }
  else if (g_state == STATE_READING && g_book_reader)
  {
    g_menu_epd4_ready = false;
    Paint_NewImage(g_image_buffer, EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT, 0, WHITE);
    Paint_SetScale(2);
    Paint_Clear(WHITE);

    g_book_reader->render();

    char page_bat[48];
    snprintf(page_bat, sizeof(page_bat), "  [%d/%d]  Bat:%d%%", g_book_reader->current_page() + 1, g_book_reader->total_pages_in_section(), g_power->battery_percent());
    Paint_DrawLine(0, EPD_3IN97_HEIGHT - 24, EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT - 24, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    WaveshareRenderer::draw_string(8, EPD_3IN97_HEIGHT - 20, g_book_reader->title(), &Font12, WHITE, BLACK);
    int title_px = strlen(g_book_reader->title()) * Font12.Width + 8;
    Paint_DrawString_EN(title_px, EPD_3IN97_HEIGHT - 20, page_bat, &Font12, WHITE, BLACK);

    EPD_3IN97_Init_Fast();
    EPD_3IN97_Display_Fast(g_image_buffer);
  }
}

static int  btn_up_prev   = HIGH;
static int  btn_sel_prev  = HIGH;
static int  btn_down_prev = HIGH;

void handle_buttons()
{
  int up   = digitalRead(BTN_UP);
  int sel  = digitalRead(BTN_SELECT);
  int down = digitalRead(BTN_DOWN);

  if (btn_up_prev == HIGH && up == LOW)
  {
    delay(DEBOUNCE_MS);
    up = digitalRead(BTN_UP);
    if (up == LOW)
    {
      Serial.println("[BTN] UP pressed");
      g_last_activity = millis();
      if (g_state == STATE_FILE_LIST)
      {
        const int old_sel  = g_file_list->selected_index();
        const int old_page = old_sel / FL_PER_PAGE;

        g_file_list->prev();
        const int new_sel  = g_file_list->selected_index();
        const int new_page = new_sel / FL_PER_PAGE;

        if (old_page != new_page || !g_menu_epd4_ready || g_file_list->needs_cover_load())
          g_needs_redraw = true;
        else
        {
          g_file_list->update_selection(g_image_buffer, old_sel, new_sel);
          EPD_3IN97_Display_Fast(g_image_buffer);
          g_needs_redraw = false;
        }
      }
      else if (g_state == STATE_READING && g_book_reader)
      {
        g_book_reader->prev();
        save_reading_progress();
        g_needs_redraw = true;
      }
    }
  }
  btn_up_prev = up;

  if (btn_down_prev == HIGH && down == LOW)
  {
    delay(DEBOUNCE_MS);
    down = digitalRead(BTN_DOWN);
    if (down == LOW)
    {
      Serial.println("[BTN] DOWN pressed");
      g_last_activity = millis();
      if (g_state == STATE_FILE_LIST)
      {
        const int old_sel  = g_file_list->selected_index();
        const int old_page = old_sel / FL_PER_PAGE;

        g_file_list->next();
        const int new_sel  = g_file_list->selected_index();
        const int new_page = new_sel / FL_PER_PAGE;

        if (old_page != new_page || !g_menu_epd4_ready || g_file_list->needs_cover_load())
          g_needs_redraw = true;
        else
        {
          g_file_list->update_selection(g_image_buffer, old_sel, new_sel);
          EPD_3IN97_Display_Fast(g_image_buffer);
          g_needs_redraw = false;
        }
      }
      else if (g_state == STATE_READING && g_book_reader)
      {
        g_book_reader->next();
        save_reading_progress();
        g_needs_redraw = true;
      }
    }
  }
  btn_down_prev = down;

  if (btn_sel_prev == HIGH && sel == LOW)
  {
    delay(DEBOUNCE_MS);
    sel = digitalRead(BTN_SELECT);
    if (sel == LOW)
    {
      Serial.println("[BTN] SELECT pressed");
      g_last_activity = millis();

      if (g_state == STATE_READING)
      {
        Serial.println("[BTN] Returning to menu");
        save_reading_progress();
        g_state        = STATE_FILE_LIST;
        g_needs_redraw = true;
      }
      else if (g_state == STATE_FILE_LIST)
      {
        const char *path = g_file_list->selected_path();
        Serial.printf("[BTN] Opening: %s\n", path ? path : "(null)");
        if (path)
        {
          Paint_NewImage(g_image_buffer, EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT, 0, WHITE);
          Paint_SetScale(2);
          Paint_Clear(WHITE);
          Paint_DrawString_EN(240, 220, "Loading book...", &Font24, WHITE, BLACK);
          EPD_3IN97_Init_Fast();
          EPD_3IN97_Display_Fast(g_image_buffer);

          request_book_load(path);
          while (!g_load_done) { delay(50); }

          if (g_load_success)
          {
            Serial.println("[BTN] Book loaded");
            String key_sect = "sect_" + String(g_file_list->selected_index());
            String key_page = "page_" + String(g_file_list->selected_index());
            int sect = g_prefs.getInt(key_sect.c_str(), 0);
            int page = g_prefs.getInt(key_page.c_str(), 0);
            g_book_reader->go_to(sect, page);
            g_prefs.putString("last_book", path);
            g_state = STATE_READING;
            g_needs_redraw = true;
          }
          else
          {
            Serial.println("[BTN] Book load failed");
            Paint_Clear(WHITE);
            Paint_DrawString_EN(200, 220, "Failed to open book", &Font20, WHITE, BLACK);
            EPD_3IN97_Init_Fast();
            EPD_3IN97_Display_Fast(g_image_buffer);
            delay(2000);
            g_needs_redraw = true;
          }
        }
      }
    }
  }
  btn_sel_prev = sel;
}

void save_reading_progress()
{
  if (!g_book_reader) return;
  int idx  = g_file_list->selected_index();
  String key_sect = "sect_" + String(idx);
  String key_page = "page_" + String(idx);
  g_prefs.putInt(key_sect.c_str(), g_book_reader->current_section());
  g_prefs.putInt(key_page.c_str(), g_book_reader->current_page());
  g_prefs.putString("last_book", g_book_reader->path());
  g_prefs.putInt("last_sect", g_book_reader->current_section());
  g_prefs.putInt("last_page", g_book_reader->current_page());
}

void show_cover_and_sleep()
{
  Serial.println("[SLEEP] Preparing sleep screen...");
  save_reading_progress();

  bool cover_shown = false;

  if (g_book_reader)
  {
    const char *book_path = g_book_reader->path();
    std::string lp = book_path;
    std::transform(lp.begin(), lp.end(), lp.begin(), ::tolower);

    if (lp.rfind(".epub") != std::string::npos)
    {
      Serial.println("[SLEEP] Requesting cover load on background task...");
      strncpy(g_load_path, book_path, sizeof(g_load_path) - 1);
      g_job_type = JOB_SLEEP_COVER;
      g_load_done = false;
      g_load_success = false;
      xSemaphoreGive(g_load_sem);
      while (!g_load_done) { delay(20); }

      uint8_t *cover = g_sleep_cover_buf;
      g_sleep_cover_buf = nullptr;

      if (cover)
      {
        memset(g_image_buf4, 0xFF, (EPD_3IN97_WIDTH / 4) * EPD_3IN97_HEIGHT);

        int cx = (EPD_3IN97_WIDTH  - COVER_W) / 2;
        int cy = (EPD_3IN97_HEIGHT - COVER_H) / 2 - 20;

        int fb_stride = EPD_3IN97_WIDTH / 4;
        for (int y = 0; y < COVER_H; y++)
        {
          int fy = cy + y;
          if (fy < 0 || fy >= EPD_3IN97_HEIGHT) continue;
          for (int x = 0; x < COVER_W; x++)
          {
            int fx = cx + x;
            if (fx < 0 || fx >= EPD_3IN97_WIDTH) continue;
            int src_byte  = y * COVER_STRIDE + x / 4;
            int src_shift = 6 - (x % 4) * 2;
            uint8_t g4    = (cover[src_byte] >> src_shift) & 0x03;
            int dst_byte  = fy * fb_stride + fx / 4;
            int dst_shift = 6 - (fx % 4) * 2;
            g_image_buf4[dst_byte] &= ~(0x03 << dst_shift);
            g_image_buf4[dst_byte] |=  (g4   << dst_shift);
          }
        }
        free(cover);

        Paint_NewImage(g_image_buf4, EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT, 0, GRAY1);
        Paint_SetScale(4);
        const char *t = g_book_reader->title();
        int tw = strlen(t) * Font16.Width;
        int tx = std::max(8, (EPD_3IN97_WIDTH - tw) / 2);
        WaveshareRenderer::draw_string(tx, cy + COVER_H + 10, t, &Font16, GRAY1, GRAY4);

        EPD_3IN97_Init_4GRAY();
        EPD_3IN97_Display_4Gray(g_image_buf4);
        cover_shown = true;
      }
    }
  }

  if (!cover_shown)
  {
    Paint_NewImage(g_image_buffer, EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT, 0, WHITE);
    Paint_SetScale(2);
    Paint_Clear(WHITE);
    Paint_DrawString_EN(280, 200, "Sleeping...", &Font24, WHITE, BLACK);
    EPD_3IN97_Init();
    EPD_3IN97_Display(g_image_buffer);
  }

  EPD_3IN97_Sleep();
  DEV_Delay_ms(500);
  DEV_Module_Exit();

  esp_sleep_enable_ext1_wakeup((1ULL << BTN_SELECT), ESP_EXT1_WAKEUP_ALL_LOW);
  esp_deep_sleep_start();
}
