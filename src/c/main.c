#include <pebble.h>

#define MAX_STOPS 4
#define MAX_ROWS 10
#define BRAND_BG PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorBlack)
#define BRAND_HI PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorBlack)
#define LIVE_FG PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorBlack)

typedef struct { char *route, *head, *due; bool live; } Bus;
typedef struct { char *name, *code; Bus rows[MAX_ROWS]; int n; } Stop;

static Window *s_window;
static MenuLayer *s_menu;
static TextLayer *s_status;
static char s_buf[2048];
static char s_status_text[96] = "Fetching buses…";
static Stop s_stops[MAX_STOPS];
static int s_nstops;

// Splits s at sep in place; returns the next piece or NULL.
static char *cut(char **s, char sep) {
  if (!*s) return NULL;
  char *start = *s, *end = strchr(start, sep);
  if (end) { *end = '\0'; *s = end + 1; } else { *s = NULL; }
  return start;
}

// Format from pkjs: stops split by \x1e, lines by \n, fields by \t.
static void parse(const char *data) {
  strncpy(s_buf, data, sizeof(s_buf) - 1);
  s_nstops = 0;
  char *rest = s_buf, *block;
  while ((block = cut(&rest, '\x1e')) && s_nstops < MAX_STOPS) {
    Stop *st = &s_stops[s_nstops++];
    char *line = cut(&block, '\n');
    st->name = cut(&line, '\t');
    st->code = line ? line : "";
    st->n = 0;
    while ((line = cut(&block, '\n')) && st->n < MAX_ROWS) {
      Bus *b = &st->rows[st->n++];
      b->route = cut(&line, '\t');
      b->head = cut(&line, '\t');
      b->due = cut(&line, '\t');
      b->live = line && line[0] == '1';
      if (!b->head || !b->due) st->n--;  // malformed line
    }
  }
}

static uint16_t num_sections(MenuLayer *m, void *ctx) { return s_nstops; }

static uint16_t num_rows(MenuLayer *m, uint16_t section, void *ctx) {
  return s_stops[section].n ? s_stops[section].n : 1;
}

static int16_t header_height(MenuLayer *m, uint16_t section, void *ctx) { return 30; }

static int16_t cell_height(MenuLayer *m, MenuIndex *i, void *ctx) {
  return PBL_IF_ROUND_ELSE(menu_layer_is_index_selected(m, i) ? 52 : 40, 46);
}

static void draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *data) {
  GRect b = layer_get_bounds(cell);
  Stop *st = &s_stops[section];
  graphics_context_set_fill_color(ctx, BRAND_BG);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  char title[64];
  snprintf(title, sizeof(title), "%s · %s", st->name, st->code);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(4, 2, b.size.w - 8, 24), GTextOverflowModeTrailingEllipsis,
                     PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft), NULL);
}

static void draw_row(GContext *ctx, const Layer *cell, MenuIndex *i, void *data) {
  GRect b = layer_get_bounds(cell);
  Stop *st = &s_stops[i->section];
  bool sel = menu_cell_layer_is_highlighted(cell);
  GColor fg = sel ? GColorBlack : GColorWhite;
  graphics_context_set_text_color(ctx, fg);
  int pad = PBL_IF_ROUND_ELSE(b.size.w / 8, 4);

  if (!st->n) {
    graphics_draw_text(ctx, "No buses in the next 90 min", fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       GRect(pad, 4, b.size.w - 2 * pad, b.size.h - 4), GTextOverflowModeWordWrap,
                       GTextAlignmentCenter, NULL);
    return;
  }
  Bus *bus = &st->rows[i->row];
  GFont big = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  int due_w = 64;
  graphics_draw_text(ctx, bus->route, big, GRect(pad, -2, 60, 28),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, bus->due, big, GRect(b.size.w - pad - due_w, -2, due_w, 28),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);
  graphics_draw_text(ctx, bus->head, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(pad, 24, b.size.w - 2 * pad - 14, 18), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
  if (bus->live) {  // live prediction marker; scheduled-only rows have none
    graphics_context_set_fill_color(ctx, sel ? GColorBlack : LIVE_FG);
    graphics_fill_circle(ctx, GPoint(b.size.w - pad - 4, 33), 3);
  }
}

static void select_click(MenuLayer *m, MenuIndex *i, void *ctx) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return;
  dict_write_uint8(it, MESSAGE_KEY_REFRESH, 1);
  app_message_outbox_send();
  vibes_short_pulse();
}

static void show(bool list) {
  layer_set_hidden(menu_layer_get_layer(s_menu), !list);
  layer_set_hidden(text_layer_get_layer(s_status), list);
}

static void inbox(DictionaryIterator *it, void *ctx) {
  Tuple *t = dict_find(it, MESSAGE_KEY_DATA);
  if (t) {
    parse(t->value->cstring);
    menu_layer_reload_data(s_menu);
    show(s_nstops > 0);
  }
  if ((t = dict_find(it, MESSAGE_KEY_STATUS))) {
    strncpy(s_status_text, t->value->cstring, sizeof(s_status_text) - 1);
    text_layer_set_text(s_status, s_status_text);
    show(false);
  }
}

static void window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  window_set_background_color(w, GColorBlack);

  s_menu = menu_layer_create(b);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
    .get_num_sections = num_sections, .get_num_rows = num_rows,
    .get_header_height = header_height, .draw_header = draw_header,
    .get_cell_height = cell_height, .draw_row = draw_row, .select_click = select_click,
  });
  menu_layer_set_normal_colors(s_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_menu, BRAND_HI, GColorBlack);
  menu_layer_set_click_config_onto_window(s_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_menu));

  int inset = PBL_IF_ROUND_ELSE(30, 10);
  s_status = text_layer_create(GRect(inset, b.size.h / 2 - 40, b.size.w - 2 * inset, 80));
  text_layer_set_background_color(s_status, GColorClear);
  text_layer_set_text_color(s_status, GColorWhite);
  text_layer_set_font(s_status, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_status, GTextAlignmentCenter);
  text_layer_set_text(s_status, s_status_text);
  layer_add_child(root, text_layer_get_layer(s_status));
  show(false);
}

static void window_unload(Window *w) {
  menu_layer_destroy(s_menu);
  text_layer_destroy(s_status);
}

int main(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){.load = window_load, .unload = window_unload});
  app_message_register_inbox_received(inbox);
  app_message_open(sizeof(s_buf) + 64, 64);
  window_stack_push(s_window, true);
  app_event_loop();
  window_destroy(s_window);
}
