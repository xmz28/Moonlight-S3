#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include "config.h"
#include "display.h"
#include "esp_jpeg_dec.h"
#include "font.h"

namespace display {
static esp_lcd_panel_handle_t panel;
static uint16_t *fb[2], *small;
static SemaphoreHandle_t complete;
static jpeg_dec_handle_t decoder;
static int front;
static bool healthy=true;
static bool IRAM_ATTR frameComplete(esp_lcd_panel_handle_t,
    const esp_lcd_rgb_panel_event_data_t *, void *) {
  BaseType_t wake=pdFALSE;
  xSemaphoreGiveFromISR(complete, &wake);
  return wake==pdTRUE;
}
bool begin() {
  complete=xSemaphoreCreateBinary();
  if (!complete) return false;
  esp_lcd_rgb_panel_config_t c={};
  c.clk_src=LCD_CLK_SRC_DEFAULT;
  c.timings.pclk_hz=config::PixelClock;
  c.timings.h_res=config::Width; c.timings.v_res=config::Height;
  c.timings.hsync_pulse_width=48; c.timings.hsync_back_porch=40;
  c.timings.hsync_front_porch=8; c.timings.vsync_pulse_width=2;
  c.timings.vsync_back_porch=40; c.timings.vsync_front_porch=6;
  c.timings.flags.pclk_active_neg=true;
  c.data_width=16; c.bits_per_pixel=16; c.num_fbs=2;
  c.bounce_buffer_size_px=config::Width*12; c.dma_burst_size=64;
  c.hsync_gpio_num=config::Hsync; c.vsync_gpio_num=config::Vsync;
  c.de_gpio_num=config::De; c.pclk_gpio_num=config::Pclk; c.disp_gpio_num=-1;
  for (int i=0;i<16;++i) c.data_gpio_nums[i]=config::DataPins[i];
  c.flags.fb_in_psram=true;
  if (esp_lcd_new_rgb_panel(&c,&panel)!=ESP_OK) return false;
  void *raw[2]={};
  if (esp_lcd_rgb_panel_get_frame_buffer(panel,2,&raw[0],&raw[1])!=ESP_OK) return false;
  fb[0]=static_cast<uint16_t *>(raw[0]); fb[1]=static_cast<uint16_t *>(raw[1]);
  for (auto buffer : fb) {
    if (!buffer || (reinterpret_cast<uintptr_t>(buffer)&15)) return false;
    memset(buffer,0,config::Width*config::Height*2);
  }
  small=static_cast<uint16_t *>(heap_caps_aligned_alloc(16,400*240*2,
      MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if (!small) return false;
  esp_lcd_rgb_panel_event_callbacks_t callbacks={};
  callbacks.on_frame_buf_complete=frameComplete;
  if (esp_lcd_rgb_panel_register_event_callbacks(panel,&callbacks,nullptr)!=ESP_OK ||
      esp_lcd_panel_reset(panel)!=ESP_OK || esp_lcd_panel_init(panel)!=ESP_OK) return false;
  jpeg_dec_config_t j=DEFAULT_JPEG_DEC_CONFIG();
  j.output_type=JPEG_PIXEL_FORMAT_RGB565_LE;
  if (jpeg_dec_open(&j,&decoder)!=JPEG_ERR_OK) return false;
  pinMode(config::Backlight,OUTPUT); digitalWrite(config::Backlight,HIGH);
  return true;
}
static bool flip(int back) {
  while (xSemaphoreTake(complete,0)==pdTRUE) {}
  if (esp_lcd_panel_draw_bitmap(panel,0,0,800,480,fb[back])!=ESP_OK) return false;
  // Never write either DMA buffer again after an unconfirmed switch.
  if (xSemaphoreTake(complete,pdMS_TO_TICKS(200))!=pdTRUE) {
    healthy=false;
    Serial.println("LCD frame-complete timeout: restart required");
    return false;
  }
  front=back;
  return true;
}
static void text(int x,int y,const char *str,uint16_t color,int scale=2) {
  for (;*str;++str,x+=8*scale) {
    unsigned ch=static_cast<uint8_t>(*str);
    if (ch<32 || ch>126) ch='?';
    for (int row=0;row<16;++row) for (int col=0;col<8;++col)
      if (Font[ch-32][row] & (1<<(7-col)))
        for (int dy=0;dy<scale;++dy) for(int dx=0;dx<scale;++dx) {
          const int xx=x+col*scale+dx, yy=y+row*scale+dy;
          if (xx>=0 && xx<800 && yy>=0 && yy<480) fb[front^1][yy*800+xx]=color;
        }
  }
}
void splash(const char *network,const char *ip) {
  memset(fb[front^1],0,800*480*2);
  text(60,50,"MOONLIGHT S3",0x07ff,3);
  text(60,140,network,0xffff);
  text(60,200,ip,0xffff);
  text(60,280,"Start Moonlight S3 Host on PC",0x07e0);
  text(60,340,"JPEG desktop + touch control",0xffff);
  flip(front^1);
}
bool present(const uint8_t *jpeg,size_t size,uint32_t &decodeUs,uint32_t &totalUs) {
  if (!healthy) return false;
  const uint32_t start=micros();
  jpeg_dec_io_t io={};
  io.inbuf=const_cast<uint8_t *>(jpeg); io.inbuf_len=size;
  jpeg_dec_header_info_t info={}; int length=0;
  if (jpeg_dec_parse_header(decoder,&io,&info)!=JPEG_ERR_OK ||
      jpeg_dec_get_outbuf_len(decoder,&length)!=JPEG_ERR_OK) return false;
  const bool low=info.width==400 && info.height==240;
  if ((!low && (info.width!=800 || info.height!=480)) ||
      length!=(low?400*240*2:800*480*2)) return false;
  const int back=front^1;
  io.outbuf=reinterpret_cast<uint8_t *>(low?small:fb[back]);
  if (jpeg_dec_process(decoder,&io)!=JPEG_ERR_OK) return false;
  decodeUs=micros()-start;
  if (low) {
    for (int y=0;y<240;++y) {
      uint16_t *row=fb[back]+y*2*800;
      for (int x=0;x<400;++x) row[x*2]=row[x*2+1]=small[y*400+x];
      memcpy(row+800,row,1600);
    }
  }
  const bool ok=flip(back);
  totalUs=micros()-start;
  return ok;
}
} // namespace display
