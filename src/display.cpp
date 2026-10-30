#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include "config.h"
#include "display.h"
#include "esp_jpeg_dec.h"

namespace display {
static esp_lcd_panel_handle_t panel;
static uint16_t *fb[3], *block;
constexpr int BlockBytes=800*16*2;
static SemaphoreHandle_t complete;
static QueueHandle_t freeBuffers;
static portMUX_TYPE submitLock=portMUX_INITIALIZER_UNLOCKED;
static jpeg_dec_handle_t decoder;
static int front;
static volatile bool healthy=true;
// Expand in internal SRAM, then stream contiguous copies to PSRAM. Writing
// individual duplicated 16-bit pixels straight to PSRAM stalls its cache.
alignas(16) static uint32_t expandedRow[400];
static bool IRAM_ATTR frameComplete(esp_lcd_panel_handle_t,
    const esp_lcd_rgb_panel_event_data_t *, void *) {
  BaseType_t wake=pdFALSE;
  xSemaphoreGiveFromISR(complete, &wake);
  return wake==pdTRUE;
}
bool begin() {
  complete=xSemaphoreCreateBinary();
  freeBuffers=xQueueCreate(2,sizeof(int));
  if (!complete || !freeBuffers) return false;
  esp_lcd_rgb_panel_config_t c={};
  c.clk_src=LCD_CLK_SRC_DEFAULT;
  c.timings.pclk_hz=config::PixelClock;
  c.timings.h_res=config::Width; c.timings.v_res=config::Height;
  c.timings.hsync_pulse_width=48; c.timings.hsync_back_porch=40;
  c.timings.hsync_front_porch=8; c.timings.vsync_pulse_width=2;
  c.timings.vsync_back_porch=40; c.timings.vsync_front_porch=6;
  c.timings.flags.pclk_active_neg=true;
  c.data_width=16; c.bits_per_pixel=16; c.num_fbs=3;
  c.bounce_buffer_size_px=config::Width*12; c.dma_burst_size=64;
  c.hsync_gpio_num=config::Hsync; c.vsync_gpio_num=config::Vsync;
  c.de_gpio_num=config::De; c.pclk_gpio_num=config::Pclk; c.disp_gpio_num=-1;
  for (int i=0;i<16;++i) c.data_gpio_nums[i]=config::DataPins[i];
  c.flags.fb_in_psram=true;
  if (esp_lcd_new_rgb_panel(&c,&panel)!=ESP_OK) return false;
  void *raw[3]={};
  if (esp_lcd_rgb_panel_get_frame_buffer(panel,3,&raw[0],&raw[1],&raw[2])!=ESP_OK) return false;
  for (int i=0;i<3;++i) fb[i]=static_cast<uint16_t *>(raw[i]);
  for (auto buffer : fb) {
    if (!buffer || (reinterpret_cast<uintptr_t>(buffer)&15)) return false;
    memset(buffer,0,config::Width*config::Height*2);
  }
  for (int i=1;i<3;++i) xQueueSend(freeBuffers,&i,0);
  block=static_cast<uint16_t *>(heap_caps_aligned_alloc(16,BlockBytes,
      MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
  if (!block) return false;
  esp_lcd_rgb_panel_event_callbacks_t callbacks={};
  callbacks.on_frame_buf_complete=frameComplete;
  if (esp_lcd_rgb_panel_register_event_callbacks(panel,&callbacks,nullptr)!=ESP_OK ||
      esp_lcd_panel_reset(panel)!=ESP_OK || esp_lcd_panel_init(panel)!=ESP_OK) return false;
  jpeg_dec_config_t j=DEFAULT_JPEG_DEC_CONFIG();
  j.output_type=JPEG_PIXEL_FORMAT_RGB565_LE;
  j.block_enable=true;
  if (jpeg_dec_open(&j,&decoder)!=JPEG_ERR_OK) return false;
  pinMode(config::Backlight,OUTPUT); digitalWrite(config::Backlight,HIGH);
  return true;
}
static bool flip(int back) {
  // LCD callbacks run on core 0, as does this presenter. Drain + submit must
  // not be split by a callback for the old buffer. In bounce-buffer mode the
  // driver only selects our existing framebuffer here (no pixel copy/wait).
  portENTER_CRITICAL(&submitLock);
  while (xSemaphoreTake(complete,0)==pdTRUE) {}
  const esp_err_t submitted=esp_lcd_panel_draw_bitmap(panel,0,0,800,480,fb[back]);
  portEXIT_CRITICAL(&submitLock);
  if (submitted!=ESP_OK) return false;
  // Never write either DMA buffer again after an unconfirmed switch.
  if (xSemaphoreTake(complete,pdMS_TO_TICKS(200))!=pdTRUE) {
    healthy=false;
    Serial.println("LCD frame-complete timeout: restart required");
    return false;
  }
  const int previous=front;
  front=back;
  xQueueSend(freeBuffers,&previous,0);
  return true;
}
uint16_t *beginUi(int &index) {
  if (!healthy || !idle() || xQueueReceive(freeBuffers,&index,0)!=pdTRUE) return nullptr;
  return fb[index];
}
void endUi(int index) { if (!flip(index)) healthy=false; }
bool idle() { return uxQueueMessagesWaiting(freeBuffers)==2; }
void discard(const Frame &frame) {
  if (frame.index>=0) xQueueSend(freeBuffers,&frame.index,0);
}
bool prepare(const uint8_t *jpeg,size_t size,Frame &frame) {
  if (!healthy) return false;
  if (xQueueReceive(freeBuffers,&frame.index,pdMS_TO_TICKS(200))!=pdTRUE) return false;
  const uint32_t start=micros();
  frame.started=start;
  jpeg_dec_io_t io={};
  io.inbuf=const_cast<uint8_t *>(jpeg); io.inbuf_len=size;
  jpeg_dec_header_info_t info={}; int length=0, blocks=0;
  if (jpeg_dec_parse_header(decoder,&io,&info)!=JPEG_ERR_OK ||
      jpeg_dec_get_outbuf_len(decoder,&length)!=JPEG_ERR_OK ||
      jpeg_dec_get_process_count(decoder,&blocks)!=JPEG_ERR_OK) return false;
  const bool low=info.width==400 && info.height==240;
  if ((!low && (info.width!=800 || info.height!=480)) ||
      length<=0 || length>BlockBytes || blocks<=0 || blocks>60) return false;
  const int back=frame.index;
  io.outbuf=reinterpret_cast<uint8_t *>(block);
  frame.decodeUs=micros()-start;
  int outputY=0;
  for (int part=0;part<blocks;++part) {
    const uint32_t decoding=micros();
    if (jpeg_dec_process(decoder,&io)!=JPEG_ERR_OK) return false;
    frame.decodeUs+=micros()-decoding;
    if (io.out_size<=0 || io.out_size>length || io.out_size%(info.width*2)) return false;
    const int rows=io.out_size/(info.width*2);
    if (outputY+rows>info.height) return false;
    if (low) {
      for (int y=0;y<rows;++y) {
        uint16_t *row=fb[back]+(outputY+y)*2*800;
        const uint16_t *source=block+y*400;
        for (int x=0;x<400;++x) {
          const uint32_t pixel=source[x];
          expandedRow[x]=pixel|(pixel<<16);
        }
        memcpy(row,expandedRow,sizeof(expandedRow));
        memcpy(row+800,expandedRow,sizeof(expandedRow));
      }
    } else {
      memcpy(fb[back]+outputY*800,block,io.out_size);
    }
    outputY+=rows;
  }
  if (outputY!=info.height) return false;
  frame.preparedUs=micros()-start;
  return true;
}
bool present(const Frame &frame,uint32_t &totalUs) {
  if (!healthy) return false;
  const bool ok=flip(frame.index);
  if (!ok) healthy=false;
  totalUs=micros()-frame.started;
  static uint32_t count, decoded, expanded, waited;
  decoded+=frame.decodeUs; expanded+=frame.preparedUs-frame.decodeUs;
  waited+=totalUs-frame.preparedUs;
  if (++count==64) {
    Serial.printf("render64 decode=%lu us expand=%lu us wait=%lu us\n",
      decoded/count,expanded/count,waited/count);
    count=decoded=expanded=waited=0;
  }
  return ok;
}
} // namespace display
