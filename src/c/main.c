#include <pebble.h>

#define MAX_STOPS 4
#define MAX_ROWS 10
// TFI livery: Dublin Bus yellow + TFI green (nearest Pebble palette colours).
#define TFI_YELLOW GColorYellow
#define TFI_GREEN GColorJaegerGreen
#define LIVE_FG GColorDarkGreen

typedef struct { char *route, *head, *due; bool live; } Bus;
typedef struct { char *name, *code; Bus rows[MAX_ROWS]; int n; } Stop;

static Window *s_window;
static MenuLayer *s_menu;
static TextLayer *s_status;
static StatusBarLayer *s_bar;
static char s_updated[24] = "";
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
  if (section >= s_nstops) return 0;  // MenuLayer queries before any data arrives
  return s_stops[section].n ? s_stops[section].n : 1;
}

static int16_t header_height(MenuLayer *m, uint16_t section, void *ctx) { return 38; }

static int16_t cell_height(MenuLayer *m, MenuIndex *i, void *ctx) {
  return s_stops[i->section].n ? 42 : 84;
}

static void draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *data) {
  GRect b = layer_get_bounds(cell);
  Stop *st = &s_stops[section];
  graphics_context_set_fill_color(ctx, TFI_YELLOW);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, 36, b.size.w, 2), 0, GCornerNone);
  char sub[48];
  snprintf(sub, sizeof(sub), "Stop %s · %s", st->code, s_updated);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, st->name, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(6, -1, b.size.w - 12, 22), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, sub, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(6, 17, b.size.w - 12, 18), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
}

// Dot plus two signal arcs, 11x11, top-left at (x, y).
static void draw_live(GContext *ctx, int x, int y, GColor c) {
  graphics_context_set_fill_color(ctx, c);
  graphics_context_set_stroke_color(ctx, c);
  graphics_fill_circle(ctx, GPoint(x + 2, y + 9), 2);
  graphics_draw_arc(ctx, GRect(x - 4, y + 3, 12, 12), GOvalScaleModeFitCircle, 0, DEG_TO_TRIGANGLE(90));
  graphics_draw_arc(ctx, GRect(x - 9, y - 2, 22, 22), GOvalScaleModeFitCircle, 0, DEG_TO_TRIGANGLE(90));
}

static void draw_row(GContext *ctx, const Layer *cell, MenuIndex *i, void *data) {
  GRect b = layer_get_bounds(cell);
  Stop *st = &s_stops[i->section];
  graphics_context_set_text_color(ctx, GColorBlack);

  if (!st->n) {
    graphics_draw_text(ctx, "No buses", fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                       GRect(8, 14, b.size.w - 16, 28), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    graphics_context_set_text_color(ctx, GColorDarkGray);
    graphics_draw_text(ctx, "in the next 90 min", fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       GRect(8, 42, b.size.w - 16, 24), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }
  Bus *bus = &st->rows[i->row];
  GColor dim = bus->live ? GColorBlack : GColorDarkGray;

  graphics_context_set_fill_color(ctx, TFI_GREEN);
  graphics_fill_rect(ctx, GRect(4, 9, 42, 24), 3, GCornersAll);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, bus->route, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(4, 10, 42, 22), GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, bus->head[0] ? bus->head : "Extra bus", fonts_get_system_font(FONT_KEY_GOTHIC_18),
                     GRect(52, 0, 92, 22), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  if (bus->live) {
    draw_live(ctx, 52, 24, LIVE_FG);
    graphics_context_set_text_color(ctx, LIVE_FG);
    graphics_draw_text(ctx, "Live", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(66, 20, 60, 18), GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  } else {
    graphics_context_set_text_color(ctx, GColorDarkGray);
    graphics_draw_text(ctx, "Scheduled", fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(52, 20, 90, 18), GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  }

  // "N min" -> big number over small "min"; "Due" and "HH:MM" as one line.
  GRect right = GRect(146, 0, 50, 42);
  graphics_context_set_text_color(ctx, dim);
  char *sp = strchr(bus->due, ' ');
  if (sp) {
    char num[4];
    int len = sp - bus->due < 3 ? sp - bus->due : 3;
    memcpy(num, bus->due, len);
    num[len] = '\0';
    graphics_draw_text(ctx, num, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                       GRect(right.origin.x, -4, right.size.w, 28), GTextOverflowModeFill, GTextAlignmentRight, NULL);
    graphics_draw_text(ctx, "min", fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(right.origin.x, 20, right.size.w, 18), GTextOverflowModeFill, GTextAlignmentRight, NULL);
  } else {
    bool clock = strchr(bus->due, ':') != NULL;
    graphics_draw_text(ctx, bus->due, fonts_get_system_font(clock ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_24_BOLD),
                       GRect(right.origin.x, clock ? 8 : 4, right.size.w, 28), GTextOverflowModeFill, GTextAlignmentRight, NULL);
  }

  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_draw_line(ctx, GPoint(0, 41), GPoint(b.size.w, 41));
}

static void select_click(MenuLayer *m, MenuIndex *i, void *ctx) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return;
  dict_write_uint8(it, MESSAGE_KEY_REFRESH, 1);
  app_message_outbox_send();
  strncpy(s_updated, "refreshing…", sizeof(s_updated));
  menu_layer_reload_data(s_menu);
  vibes_short_pulse();
}

static void show(bool list, GColor bg, GColor fg) {
  layer_set_hidden(menu_layer_get_layer(s_menu), !list);
  layer_set_hidden(text_layer_get_layer(s_status), list);
  window_set_background_color(s_window, list ? GColorWhite : bg);
  text_layer_set_text_color(s_status, fg);
}

static void inbox(DictionaryIterator *it, void *ctx) {
  Tuple *t = dict_find(it, MESSAGE_KEY_DATA);
  if (t) {
    parse(t->value->cstring);
    char hhmm[8];
    clock_copy_time_string(hhmm, sizeof(hhmm));
    snprintf(s_updated, sizeof(s_updated), "updated %s", hhmm);
    menu_layer_reload_data(s_menu);
    show(s_nstops > 0, GColorWhite, GColorBlack);
  }
  if ((t = dict_find(it, MESSAGE_KEY_STATUS))) {  // only sent for API key problems
    strncpy(s_status_text, t->value->cstring, sizeof(s_status_text) - 1);
    text_layer_set_text(s_status, s_status_text);
    show(false, TFI_YELLOW, GColorBlack);
  }
}

static void window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  int bar = STATUS_BAR_LAYER_HEIGHT;

  s_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_bar, TFI_GREEN, GColorWhite);
  layer_add_child(root, status_bar_layer_get_layer(s_bar));

  s_menu = menu_layer_create(GRect(0, bar, b.size.w, b.size.h - bar));
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
    .get_num_sections = num_sections, .get_num_rows = num_rows,
    .get_header_height = header_height, .draw_header = draw_header,
    .get_cell_height = cell_height, .draw_row = draw_row, .select_click = select_click,
  });
  menu_layer_set_normal_colors(s_menu, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_menu, TFI_YELLOW, GColorBlack);
  menu_layer_set_click_config_onto_window(s_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_menu));

  int inset = PBL_IF_ROUND_ELSE(30, 10);
  s_status = text_layer_create(GRect(inset, b.size.h / 2 - 40, b.size.w - 2 * inset, 90));
  text_layer_set_background_color(s_status, GColorClear);
  text_layer_set_font(s_status, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_status, GTextAlignmentCenter);
  text_layer_set_text(s_status, s_status_text);
  layer_add_child(root, text_layer_get_layer(s_status));
  show(false, GColorWhite, GColorBlack);
}

static void window_unload(Window *w) {
  menu_layer_destroy(s_menu);
  text_layer_destroy(s_status);
  status_bar_layer_destroy(s_bar);
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
