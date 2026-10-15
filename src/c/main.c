#include <pebble.h>

#define MAX_ROWS 10
#define MAX_HOME 24
// TFI livery: Dublin Bus yellow + TFI green (nearest Pebble palette colours).
#define TFI_YELLOW GColorYellow
#define TFI_GREEN GColorJaegerGreen
#define LIVE_FG GColorDarkGreen

typedef struct { char *route, *head, *due; bool live; } Bus;
typedef struct { char kind, *code, *name, *dist, *routes; bool fav; } Place;

// ---- Messages from pkjs (see src/pkjs/index.js for the formats) ----

static char s_home_buf[2048], s_dep_buf[1024];
static Place s_places[MAX_HOME];
static int s_nfav, s_nnear;
static char s_near_state[64] = "loading";

static char *s_name = "", *s_code = "", *s_dist = "";
static bool s_fav, s_loaded;
static Bus s_rows[MAX_ROWS];
static int s_nrows;
static char s_status_text[96];
static char s_updated[24];

// Splits s at sep in place; returns the next piece or NULL.
static char *cut(char **s, char sep) {
  if (!*s) return NULL;
  char *start = *s, *end = strchr(start, sep);
  if (end) { *end = '\0'; *s = end + 1; } else { *s = NULL; }
  return start;
}

static char *field(char **line) {
  char *f = cut(line, '\t');
  return f ? f : "";
}

static void parse_home(const char *data) {
  strncpy(s_home_buf, data, sizeof(s_home_buf) - 1);
  s_nfav = s_nnear = 0;
  Place favs[MAX_HOME], near[MAX_HOME];
  char *rest = s_home_buf, *line;
  while ((line = cut(&rest, '\n'))) {
    char *kind = field(&line);
    if (kind[0] == 'S') {
      strncpy(s_near_state, field(&line), sizeof(s_near_state) - 1);
      continue;
    }
    Place p = {kind[0], field(&line), field(&line), field(&line), field(&line), false};
    p.fav = field(&line)[0] == '1';
    if (p.kind == 'F' && s_nfav + s_nnear < MAX_HOME) favs[s_nfav++] = p;
    else if (p.kind == 'N' && s_nfav + s_nnear < MAX_HOME) near[s_nnear++] = p;
  }
  if (s_nnear) s_near_state[0] = '\0';
  memcpy(s_places, favs, s_nfav * sizeof(Place));
  memcpy(s_places + s_nfav, near, s_nnear * sizeof(Place));
}

static void parse_departures(const char *data) {
  strncpy(s_dep_buf, data, sizeof(s_dep_buf) - 1);
  char *rest = s_dep_buf, *line = cut(&rest, '\n');
  s_name = field(&line);
  s_code = field(&line);
  s_dist = field(&line);
  s_fav = field(&line)[0] == '1';
  s_nrows = 0;
  while ((line = cut(&rest, '\n')) && s_nrows < MAX_ROWS) {
    Bus *b = &s_rows[s_nrows++];
    b->route = field(&line);
    b->head = field(&line);
    b->due = field(&line);
    b->live = field(&line)[0] == '1';
  }
  s_loaded = true;
}

static void send_kv(uint32_t key, const char *str) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return;
  if (str) dict_write_cstring(it, key, str); else dict_write_uint8(it, key, 1);
  app_message_outbox_send();
}

// ---- Shared drawing ----

static void draw_star(GContext *ctx, GPoint o, int size, GColor c, bool filled) {
  // 5-point star in a size x size box.
  GPoint pts[10];
  static const int8_t unit[10][2] = {{50, 0}, {62, 36}, {100, 38}, {69, 61}, {80, 98},
                                     {50, 76}, {20, 98}, {31, 61}, {0, 38}, {38, 36}};
  for (int i = 0; i < 10; i++) {
    pts[i] = GPoint(o.x + unit[i][0] * size / 100, o.y + unit[i][1] * size / 100);
  }
  GPathInfo info = {10, pts};
  GPath *path = gpath_create(&info);
  if (filled) {
    graphics_context_set_fill_color(ctx, c);
    gpath_draw_filled(ctx, path);
  } else {
    graphics_context_set_stroke_color(ctx, c);
    graphics_context_set_stroke_width(ctx, 2);
    gpath_draw_outline(ctx, path);
    graphics_context_set_stroke_width(ctx, 1);
  }
  gpath_destroy(path);
}

static void text(GContext *ctx, const char *s, const char *font, GRect r, GTextAlignment a) {
  graphics_draw_text(ctx, s, fonts_get_system_font(font), r, GTextOverflowModeTrailingEllipsis, a, NULL);
}

static StatusBarLayer *add_status_bar(Layer *root) {
  StatusBarLayer *bar = status_bar_layer_create();
  status_bar_layer_set_colors(bar, TFI_GREEN, GColorWhite);
  layer_add_child(root, status_bar_layer_get_layer(bar));
  return bar;
}

static MenuLayer *add_menu(Window *w, Layer *root, MenuLayerCallbacks cb) {
  GRect b = layer_get_bounds(root);
  int bar = STATUS_BAR_LAYER_HEIGHT;
  MenuLayer *m = menu_layer_create(GRect(0, bar, b.size.w, b.size.h - bar));
  menu_layer_set_callbacks(m, NULL, cb);
  menu_layer_set_normal_colors(m, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(m, TFI_YELLOW, GColorBlack);
  menu_layer_set_click_config_onto_window(m, w);
  layer_add_child(root, menu_layer_get_layer(m));
  return m;
}

// ---- Departures window ----

static Window *s_dep_window;
static MenuLayer *s_dep_menu;
static StatusBarLayer *s_dep_bar;
static TextLayer *s_dep_status;
static Layer *s_toast;
static char s_toast_text[16];
static AppTimer *s_toast_timer;

static uint16_t dep_rows(MenuLayer *m, uint16_t section, void *ctx) { return s_nrows ? s_nrows : 1; }
static int16_t dep_header_height(MenuLayer *m, uint16_t section, void *ctx) { return 38; }
static int16_t dep_cell_height(MenuLayer *m, MenuIndex *i, void *ctx) { return s_nrows ? 42 : 84; }

static void dep_header(GContext *ctx, const Layer *cell, uint16_t section, void *data) {
  GRect b = layer_get_bounds(cell);
  graphics_context_set_fill_color(ctx, TFI_YELLOW);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, 36, b.size.w, 2), 0, GCornerNone);
  char sub[64];
  if (s_dist[0]) snprintf(sub, sizeof(sub), "Stop %s · %s · %s", s_code, s_dist, s_updated);
  else snprintf(sub, sizeof(sub), "Stop %s · %s", s_code, s_updated);
  graphics_context_set_text_color(ctx, GColorBlack);
  text(ctx, s_name, FONT_KEY_GOTHIC_18_BOLD, GRect(6, -1, 166, 22), GTextAlignmentLeft);
  text(ctx, sub, FONT_KEY_GOTHIC_14, GRect(6, 17, b.size.w - 12, 18), GTextAlignmentLeft);
  draw_star(ctx, GPoint(178, 10), 16, GColorBlack, s_fav);
}

// Dot plus two signal arcs, 11x11, top-left at (x, y).
static void draw_live(GContext *ctx, int x, int y, GColor c) {
  graphics_context_set_fill_color(ctx, c);
  graphics_context_set_stroke_color(ctx, c);
  graphics_fill_circle(ctx, GPoint(x + 2, y + 9), 2);
  graphics_draw_arc(ctx, GRect(x - 4, y + 3, 12, 12), GOvalScaleModeFitCircle, 0, DEG_TO_TRIGANGLE(90));
  graphics_draw_arc(ctx, GRect(x - 9, y - 2, 22, 22), GOvalScaleModeFitCircle, 0, DEG_TO_TRIGANGLE(90));
}

static void dep_row(GContext *ctx, const Layer *cell, MenuIndex *i, void *data) {
  GRect b = layer_get_bounds(cell);
  graphics_context_set_text_color(ctx, GColorBlack);
  if (!s_nrows) {
    text(ctx, s_loaded ? "No buses" : "Loading timetable…", FONT_KEY_GOTHIC_24_BOLD,
         GRect(8, 14, b.size.w - 16, 28), GTextAlignmentCenter);
    graphics_context_set_text_color(ctx, GColorDarkGray);
    text(ctx, s_loaded ? "in the next 90 min" : "Only slow the first time", FONT_KEY_GOTHIC_18,
         GRect(8, 42, b.size.w - 16, 24), GTextAlignmentCenter);
    return;
  }
  Bus *bus = &s_rows[i->row];

  graphics_context_set_fill_color(ctx, TFI_GREEN);
  graphics_fill_rect(ctx, GRect(4, 9, 42, 24), 3, GCornersAll);
  graphics_context_set_text_color(ctx, GColorWhite);
  text(ctx, bus->route, FONT_KEY_GOTHIC_18_BOLD, GRect(4, 10, 42, 22), GTextAlignmentCenter);

  graphics_context_set_text_color(ctx, GColorBlack);
  text(ctx, bus->head[0] ? bus->head : "Extra bus", FONT_KEY_GOTHIC_18, GRect(52, 0, 92, 22), GTextAlignmentLeft);
  if (bus->live) {
    draw_live(ctx, 52, 24, LIVE_FG);
    graphics_context_set_text_color(ctx, LIVE_FG);
    text(ctx, "Live", FONT_KEY_GOTHIC_14_BOLD, GRect(66, 20, 60, 18), GTextAlignmentLeft);
  } else {
    graphics_context_set_text_color(ctx, GColorDarkGray);
    text(ctx, "Scheduled", FONT_KEY_GOTHIC_14, GRect(52, 20, 90, 18), GTextAlignmentLeft);
  }

  // "N min" -> big number over small "min"; "Due" and "HH:MM" as one line.
  graphics_context_set_text_color(ctx, bus->live ? GColorBlack : GColorDarkGray);
  char *sp = strchr(bus->due, ' ');
  if (sp) {
    char num[4];
    int len = sp - bus->due < 3 ? sp - bus->due : 3;
    memcpy(num, bus->due, len);
    num[len] = '\0';
    text(ctx, num, FONT_KEY_GOTHIC_24_BOLD, GRect(146, -4, 50, 28), GTextAlignmentRight);
    text(ctx, "min", FONT_KEY_GOTHIC_14, GRect(146, 20, 50, 18), GTextAlignmentRight);
  } else {
    bool clock = strchr(bus->due, ':') != NULL;
    text(ctx, bus->due, clock ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_24_BOLD,
         GRect(146, clock ? 8 : 4, 50, 28), GTextAlignmentRight);
  }
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_draw_line(ctx, GPoint(0, 41), GPoint(b.size.w, 41));
}

static void dep_refresh(MenuLayer *m, MenuIndex *i, void *ctx) {
  send_kv(MESSAGE_KEY_REFRESH, NULL);
  strncpy(s_updated, "refreshing…", sizeof(s_updated));
  menu_layer_reload_data(s_dep_menu);
  vibes_short_pulse();
}

static void dep_toggle_fav(MenuLayer *m, MenuIndex *i, void *ctx) {
  if (s_code[0]) send_kv(MESSAGE_KEY_FAV, s_code);
}

static void toast_draw(Layer *l, GContext *ctx) {
  GRect b = layer_get_bounds(l);
  bool saved = s_toast_text[0] == 'S';
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 6, GCornersAll);
  draw_star(ctx, GPoint(10, 13), 20, saved ? GColorSpringBud : GColorLightGray, saved);
  graphics_context_set_text_color(ctx, GColorWhite);
  text(ctx, s_toast_text, FONT_KEY_GOTHIC_18_BOLD, GRect(40, 2, b.size.w - 46, 22), GTextAlignmentLeft);
  graphics_context_set_text_color(ctx, GColorLightGray);
  text(ctx, saved ? "Added to favourites" : "Not a favourite now", FONT_KEY_GOTHIC_14,
       GRect(40, 22, b.size.w - 46, 18), GTextAlignmentLeft);
}

static void toast_hide(void *ctx) {
  s_toast_timer = NULL;
  if (s_toast) layer_set_hidden(s_toast, true);
}

static void show_toast(const char *msg) {
  if (!s_toast) return;
  strncpy(s_toast_text, msg, sizeof(s_toast_text) - 1);
  layer_set_hidden(s_toast, false);
  layer_mark_dirty(s_toast);
  if (s_toast_timer) app_timer_cancel(s_toast_timer);
  s_toast_timer = app_timer_register(1500, toast_hide, NULL);
  vibes_short_pulse();
}

static void dep_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  window_set_background_color(w, GColorWhite);
  s_dep_bar = add_status_bar(root);
  s_dep_menu = add_menu(w, root, (MenuLayerCallbacks){
    .get_num_rows = dep_rows, .get_header_height = dep_header_height, .draw_header = dep_header,
    .get_cell_height = dep_cell_height, .draw_row = dep_row,
    .select_click = dep_refresh, .select_long_click = dep_toggle_fav,
  });
  s_dep_status = text_layer_create(GRect(10, b.size.h / 2 - 40, b.size.w - 20, 90));
  text_layer_set_background_color(s_dep_status, GColorClear);
  text_layer_set_font(s_dep_status, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_dep_status, GTextAlignmentCenter);
  layer_set_hidden(text_layer_get_layer(s_dep_status), true);
  layer_add_child(root, text_layer_get_layer(s_dep_status));
  s_toast = layer_create(GRect(14, 150, 172, 46));
  layer_set_update_proc(s_toast, toast_draw);
  layer_set_hidden(s_toast, true);
  layer_add_child(root, s_toast);
}

static void dep_unload(Window *w) {
  if (s_toast_timer) app_timer_cancel(s_toast_timer);
  s_toast_timer = NULL;
  layer_destroy(s_toast);
  s_toast = NULL;
  text_layer_destroy(s_dep_status);
  menu_layer_destroy(s_dep_menu);
  status_bar_layer_destroy(s_dep_bar);
  s_dep_menu = NULL;
  // Stop polling for this stop and re-check location for the home list.
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) == APP_MSG_OK) {
    dict_write_uint8(it, MESSAGE_KEY_CLOSE, 1);
    dict_write_uint8(it, MESSAGE_KEY_HOME, 1);
    app_message_outbox_send();
  }
}

static void open_stop(Place *p) {
  // Header from the home list until the phone answers.
  static char name[48], code[8], dist[12];
  strncpy(name, p->name, sizeof(name) - 1);
  strncpy(code, p->code, sizeof(code) - 1);
  strncpy(dist, p->dist, sizeof(dist) - 1);
  s_name = name, s_code = code, s_dist = dist, s_fav = p->fav;
  s_nrows = 0, s_loaded = false, s_updated[0] = '\0';
  send_kv(MESSAGE_KEY_OPEN, code);
  window_stack_push(s_dep_window, true);
}

// ---- Home window ----

static Window *s_home_window;
static MenuLayer *s_home_menu;
static StatusBarLayer *s_home_bar;

static uint16_t home_sections(MenuLayer *m, void *ctx) { return 2; }

static uint16_t home_rows(MenuLayer *m, uint16_t section, void *ctx) {
  if (section == 0) return s_nfav ? s_nfav : 1;
  return s_nnear ? s_nnear : 1;
}

static int16_t home_header_height(MenuLayer *m, uint16_t section, void *ctx) { return 22; }

static int16_t home_cell_height(MenuLayer *m, MenuIndex *i, void *ctx) {
  bool state = i->section == 0 ? !s_nfav : !s_nnear;
  return state ? 76 : 46;
}

static void home_header(GContext *ctx, const Layer *cell, uint16_t section, void *data) {
  GRect b = layer_get_bounds(cell);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  text(ctx, section ? "Nearby" : "Favourites", FONT_KEY_GOTHIC_14_BOLD, GRect(6, 1, 100, 18), GTextAlignmentLeft);
  graphics_context_set_text_color(ctx, GColorLightGray);
  text(ctx, section ? "within 1 km" : "hold Select to save", FONT_KEY_GOTHIC_14,
       GRect(90, 1, b.size.w - 96, 18), GTextAlignmentRight);
}

static void home_state(GContext *ctx, GRect b, const char *title, const char *sub) {
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), GRect(10, 6, b.size.w - 20, 44),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, GColorDarkGray);
  graphics_draw_text(ctx, sub, fonts_get_system_font(FONT_KEY_GOTHIC_14), GRect(10, 50, b.size.w - 20, 22),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void home_row(GContext *ctx, const Layer *cell, MenuIndex *i, void *data) {
  GRect b = layer_get_bounds(cell);
  if (i->section == 0 && !s_nfav) {
    home_state(ctx, b, "No favourites yet", "Add stops in the phone settings");
    return;
  }
  if (i->section == 1 && !s_nnear) {
    const char *st = s_near_state;
    if (!strcmp(st, "off")) home_state(ctx, b, "Turn on location for the Pebble app", "Favourites still work");
    else if (!strcmp(st, "none")) home_state(ctx, b, "No stops within 1 km", "Add one by number in phone settings");
    else if (!strcmp(st, "locating")) home_state(ctx, b, "Finding stops near you…", "Using your phone's GPS");
    else if (!strcmp(st, "loading")) home_state(ctx, b, "Loading stops…", "Only slow the first time");
    else home_state(ctx, b, st, "");  // error text from the phone
    return;
  }
  Place *p = &s_places[i->section == 0 ? i->row : s_nfav + i->row];
  int x = 6;
  if (p->kind == 'N' && p->fav) {
    draw_star(ctx, GPoint(6, 5), 14, TFI_GREEN, true);
    x = 24;
  }
  graphics_context_set_text_color(ctx, GColorBlack);
  text(ctx, p->name, FONT_KEY_GOTHIC_18_BOLD, GRect(x, 0, 140 - x - 4, 22), GTextAlignmentLeft);
  text(ctx, p->dist, FONT_KEY_GOTHIC_14_BOLD, GRect(140, 3, 54, 18), GTextAlignmentRight);
  GFont bold = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  GSize code = graphics_text_layout_get_content_size(p->code, bold, GRect(0, 0, 80, 18),
                                                      GTextOverflowModeFill, GTextAlignmentLeft);
  text(ctx, p->code, FONT_KEY_GOTHIC_14_BOLD, GRect(6, 23, 80, 18), GTextAlignmentLeft);
  if (p->routes[0]) {
    char routes[48];
    snprintf(routes, sizeof(routes), " · %s", p->routes);
    graphics_context_set_text_color(ctx, GColorDarkGray);
    text(ctx, routes, FONT_KEY_GOTHIC_14, GRect(6 + code.w, 23, b.size.w - 12 - code.w, 18), GTextAlignmentLeft);
  }
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_draw_line(ctx, GPoint(0, 45), GPoint(b.size.w, 45));
}

static void home_select(MenuLayer *m, MenuIndex *i, void *ctx) {
  if (i->section == 0 && i->row < s_nfav) open_stop(&s_places[i->row]);
  else if (i->section == 1 && i->row < s_nnear) open_stop(&s_places[s_nfav + i->row]);
}

static void home_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  window_set_background_color(w, GColorWhite);
  s_home_bar = add_status_bar(root);
  s_home_menu = add_menu(w, root, (MenuLayerCallbacks){
    .get_num_sections = home_sections, .get_num_rows = home_rows,
    .get_header_height = home_header_height, .draw_header = home_header,
    .get_cell_height = home_cell_height, .draw_row = home_row, .select_click = home_select,
  });
}

static void home_unload(Window *w) {
  menu_layer_destroy(s_home_menu);
  status_bar_layer_destroy(s_home_bar);
}

// ---- AppMessage ----

static void inbox(DictionaryIterator *it, void *ctx) {
  Tuple *t;
  if ((t = dict_find(it, MESSAGE_KEY_HOME))) {
    parse_home(t->value->cstring);
    menu_layer_reload_data(s_home_menu);
  }
  bool on_dep = window_stack_get_top_window() == s_dep_window && s_dep_menu;
  if ((t = dict_find(it, MESSAGE_KEY_DATA)) && on_dep) {
    parse_departures(t->value->cstring);
    char hhmm[8];
    clock_copy_time_string(hhmm, sizeof(hhmm));
    snprintf(s_updated, sizeof(s_updated), "updated %s", hhmm);
    layer_set_hidden(text_layer_get_layer(s_dep_status), true);
    layer_set_hidden(menu_layer_get_layer(s_dep_menu), false);
    window_set_background_color(s_dep_window, GColorWhite);
    menu_layer_reload_data(s_dep_menu);
  }
  if ((t = dict_find(it, MESSAGE_KEY_STATUS))) {
    strncpy(s_status_text, t->value->cstring, sizeof(s_status_text) - 1);
    if (on_dep) {  // key problems or a missing timetable
      text_layer_set_text(s_dep_status, s_status_text);
      layer_set_hidden(text_layer_get_layer(s_dep_status), false);
      layer_set_hidden(menu_layer_get_layer(s_dep_menu), true);
      window_set_background_color(s_dep_window, TFI_YELLOW);
    } else {
      strncpy(s_near_state, s_status_text, sizeof(s_near_state) - 1);
      s_nnear = 0;
      menu_layer_reload_data(s_home_menu);
    }
  }
  if ((t = dict_find(it, MESSAGE_KEY_TOAST)) && on_dep) show_toast(t->value->cstring);
}

int main(void) {
  s_home_window = window_create();
  window_set_window_handlers(s_home_window, (WindowHandlers){.load = home_load, .unload = home_unload});
  s_dep_window = window_create();
  window_set_window_handlers(s_dep_window, (WindowHandlers){.load = dep_load, .unload = dep_unload});
  app_message_register_inbox_received(inbox);
  app_message_open(sizeof(s_home_buf) + 64, 64);
  window_stack_push(s_home_window, true);
  app_event_loop();
  window_destroy(s_dep_window);
  window_destroy(s_home_window);
}
